#pragma once

// Durable, memory-bounded batch export (blueprint section 2, "Automatic batch development";
// section 7, "Durable, memory-bounded batch processing"; ADR 0012).
//
// A batch exports a selection of library photographs (or every photograph of an input folder) to a
// destination the user chose. It is persisted in the catalog's job journal (catalog/jobs.hpp)
// before anything is dispatched, rendered by isolated lucent-worker.exe processes
// (workers/worker_host.hpp; pixels never cross the pipe) and published by this service with a
// journaled, no-clobber protocol, so Lucent can be closed, crash or lose power at any point and
// continue without losing, duplicating or half-publishing an output.
//
// Creating a batch (submit)
//   1. Validation: the destination (BatchDestination: folder, name pattern, mirrored subfolders,
//      collision policy) and the output settings (exporter::ExportSettings: the presets JPEG / sRGB
//      / quality 95 and 16-bit TIFF / sRGB or Display P3, and the MetadataPolicy: capture EXIF,
//      artist / copyright, GPS off unless chosen). The output folder is created when missing and must
//      accept a probe file; there must be room for at least the largest single output.
//   2. Selection: the items, or a folder (imported first with the decoder registry; then every
//      library item inside it). Items that cannot be exported are excluded with their reason and
//      listed in the report: Unsupported photographs (unless allow_unsupported), unreadable
//      recipes, missing or changed sources, sources that cannot be hashed.
//   3. Freezing: each job keeps the item's recipe revision at submission (or the one the caller
//      froze earlier, BatchItem::recipe) and its SHA-256, the source's content hash (sources not
//      hashed yet are hashed now, with a stable read), the export settings, the renderer build and
//      the destination. Their combination is the job identity (catalog::compute_job_identity).
//   4. With ExistingOutputs::Skip, a job whose identity already has a receipt for a file that is
//      still in the destination folder and still hashes to that receipt is skipped ("already
//      exported") instead of being rendered again; the skipped job names that file.
//   5. The batch and all of its jobs are written in one catalog transaction and the batch starts
//      (BatchRequest::start) or stays a draft.
// plan() runs steps 1 to 4 without writing anything (no hashing, no folder creation: an existing
// output whose size matches its receipt counts as already exported, which submit() confirms by
// hashing) and returns example destination paths and counts for the user interface.
//
// Scheduling (ADR 0012, "Scheduler and memory budget")
//   * One coordinator thread claims jobs (single exports first, then the oldest running batch, then
//     batch order) and hands them to worker slots; each slot thread drives one WorkerHost (one worker process, reused for
//     its jobs, restarted after a fault) and runs the job's publication. Threads are bounded:
//     1 + max_workers.
//   * Aggregate memory budget: BatchMemoryOptions::total_bytes (default half the physical memory,
//     clamped to 2..8 GiB: 8 GiB on a 16 GB machine) minus the interactive reserve (previews and
//     the editor in Lucent.exe) is the worker budget. One worker always runs; a second (up to
//     max_workers) is admitted only after a finished job measured the real peak commit per pixel,
//     when the estimates of every running job plus the next fit in the worker budget, and when the
//     system's available memory exceeds the next estimate by free_memory_margin_bytes. Each worker
//     process has a Job Object commit limit of the worker budget, and the engine refuses a render
//     whose own estimate exceeds it: a photo too large for the budget fails with LimitExceeded (an
//     insufficient-memory result), it is never exported at a reduced size. Every worker process is
//     also nested in one parent Job Object whose job memory limit is the worker budget
//     (workers/worker_group.hpp; ADR 0012, amendment "aggregate worker memory limit"): when workers
//     together reach it (their estimates were too low), the one whose allocation was refused stops
//     with a transient failure (WorkerFault::GroupMemoryLimit) and its job is retried with no other
//     worker running (idle workers are released first). The stopped attempt is journaled as
//     abandoned and its temporary file removed, as after any other worker failure.
//   * While interactive_busy() is true (the editor is waiting for a render) no new job is admitted;
//     running jobs continue. Worker processes run below normal priority.
//   * Leases: a claimed job carries a catalog lease renewed every lease_renewal while its worker's
//     heartbeats arrive and while the coordinator verifies and publishes its output; a coordinator
//     that dies leaves leases that the next start reclaims. An attempt that ends without an outcome
//     (Lucent stopped, or the lease expired, for example while the computer slept) is an
//     interruption: requeued without using up the transient-retry budget, and bounded on its own
//     (BatchRetryPolicy::max_interruptions, then the job fails).
//   * Failures: deterministic ones (unsupported or corrupt input, a refused recipe, a changed or
//     missing source) fail the job at once with an actionable reason; transient ones (a worker crash
//     or hang, a timeout, I/O, memory pressure) are retried with exponential back-off up to
//     BatchRetryPolicy::max_transient_failures, then fail; a job the workers' aggregate memory limit
//     stopped beside another worker is always retried once, alone, whatever that budget (ADR 0012,
//     decisions 19 and 22). Destination problems (disk full, the
//     folder or its drive gone, no permission) give the job back without counting a failure and
//     pause the batch with a BatchAttention, so the remaining jobs wait instead of all failing;
//     resume() after fixing it continues. The output folder itself is never created after
//     submission (only mirrored subfolders below it): a folder deleted meanwhile is reported, and
//     one created again (another file object at that path) is used only after
//     use_replaced_folder(). Other jobs of other batches continue.
//
// Publication (ADR 0012, "Publication protocol"; journal steps in brackets)
//   [Reserved]   the final name: the pattern's name, or with CollisionPolicy::UniqueName the first
//                free one ("_2", ...; compared case-insensitively against the folder and the
//                names other running jobs reserved). The worker's temporary file is
//                "~lucent-<tag>-<job>-a<attempt>.tmp" in the final folder (same volume, unique per
//                attempt; a stale one is removed first).
//   [TempWritten] the worker rendered, encoded, wrote, re-read, fully decoded and hashed it.
//   [Verified]   the coordinator holds the file open (no writer, renamer or deleter can touch it),
//                re-hashes it through that handle (size and SHA-256 must equal the worker's), checks
//                the container header (format, dimensions equal to the rendered size, 3 channels, bit
//                depth, the embedded ICC profile is exactly the chosen profile's) and reads the EXIF
//                back (capture, rights and GPS present only as the metadata policy allows). The
//                final name is checked again and, if taken meanwhile, the next free one reserved.
//   [Published]  the held file is renamed to the final name without replacing anything (or, under
//                CollisionPolicy::Overwrite, after the existing file was revalidated against the
//                identity seen at verification and moved aside), then flushed.
//   [ReceiptCommitted] the receipt (path, size, SHA-256 and the details: settings, render summary,
//                review notes) is committed and the job completes.
//
// Recovery (when the service opens, on the coordinator thread before it dispatches anything, so
// open() never waits for a slow destination; ADR 0012, "Recovery"). Every job leased by a previous
// session is reconciled from its journal: a published output is verified against the journal or
// receipt (size and SHA-256) before the job counts as complete; an output renamed into place whose
// Published step never committed is the attempt's own verified bytes and is removed (nothing
// unrecorded stays published); temporary files, scratch folders and stale "~lucent-*.tmp" files are
// removed; the job is queued again with its interruption recorded. Nothing is decided while the
// batch's output folder cannot be reached (a drive not plugged in yet, a share not reconnected,
// another folder at its path) or an output cannot be read (a sharing violation, an I/O error): the
// job keeps its lease and is reconciled once the folder is back (checked every reclaim_interval),
// so outputs are never published twice and verified outputs are never failed for being out of
// reach. A batch that finished just before Lucent stopped gets its report files then.
//
// Automatic settings (BatchRequest::automatic; blueprint section 2, "Automatic batch development";
// section 6 steps 5-7; ADR 0013). In Auto and AnalyzeOnly batches every job is analysed in its
// worker (an analyze job, protocol version 2: app::run_automatic) from the recipe frozen at
// submission (its locks and locked values), before anything is rendered for export:
//   * The job's automatic state moves Pending -> Analysed -> Frozen in the journal
//     (catalog/jobs.hpp). Freezing writes, in ONE catalog transaction, an automatic revision per
//     attempt of the full-resolution final check up to the effective one (origin Auto, derived from
//     the frozen recipe, each with its analysis record: versions, confidence, flags with reasons,
//     compact diagnostics) and rebinds the job's revision, settings and identity to the effective
//     revision: the export identity, the output name's revision number and the receipt are the
//     effective revision's. The effective revision becomes the item's current one unless the
//     photographer edited it meanwhile (the editing rule of blueprint section 2). A low-confidence
//     result is the restrained edit, exported with its review flags in the receipt and report; a
//     photo whose final check passes no attempt fails; an undecodable or unsupported photo is
//     skipped with its reason; the others continue.
//   * Without keep_consistent the analysis and the final check run in one analyze job and the same
//     attempt goes on to export. With keep_consistent the batch first runs the related-shot
//     pre-pass: every job is analysed (analysis only) and its analysis document is recorded
//     (Analysed) before it is given back, so a restart never analyses it again; while any job of
//     the batch is still Pending, only Pending jobs are claimed. Then the groups (capture time,
//     visual similarity, illumination; BatchItem::group overrides; BatchItem::anchor marks anchors)
//     and each member's consistency target are computed from the recorded documents (deterministic,
//     so the same after a restart), and each job's second analyze job regularises it towards its
//     target and runs the final check before it is frozen and exported.
//   * AnalyzeOnly batches stop there: a frozen job completes without an output (its automatic
//     revisions are the result, editable in the editor); the report lists it as analysed with its
//     review flags.
// Existing outputs (ExistingOutputs::Skip) are not looked up for automatic jobs at submission,
// since their identity is known only once they are frozen.
//
// Pause lets running jobs finish their current attempt (their outputs are published) and
// dispatches nothing new. Cancel cancels queued jobs and asks running workers to stop (they are
// terminated after cancel_grace); outputs already published stay valid. Resume continues.
//
// Completion. When a batch finishes, a report (JSON "lucent.batch-report" v1 and a readable HTML
// page) is written into its output folder (unless write_report_files is false): exported, skipped,
// failed, cancelled and review-recommended photographs with reasons and suggested actions.
// report() returns the same data for the user interface; output_folder() is what "Open output
// folder" opens.
//
// Threading and callbacks: observers run on the coordinator or a slot thread and must return
// promptly (post to a UI thread); the library contract of library.hpp applies (they may call the
// service). submit() and plan() read files (hashing, listings): call them off the UI thread. The
// destructor stops dispatching, cancels running worker jobs and gives their jobs back (Queued), so
// a running batch continues when the service is opened again.
//
// Thread-safety: all members may be called concurrently from any thread.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/batch_destination.hpp>
#include <lucent/application/camera_support.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/catalog/jobs.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/export/export.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/workers/protocol.hpp>

namespace lucent::app {

class RecipeService;

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

// A recipe frozen when the user queued the photo (RecipeService::current_settings). The batch
// journal stores the revision and its SHA-256; settings without a revision must be the as-shot
// defaults (anything else could not be reproduced from the catalog after a restart).
struct FrozenRecipe {
    engine::DevelopSettings settings;
    std::optional<catalog::RevisionId> revision;  // std::nullopt: as shot
    std::uint32_t revision_number = 0;
};

struct BatchItem {
    ItemId item;
    std::optional<FrozenRecipe> recipe;  // std::nullopt: submit() freezes the current recipe
    // Automatic batches keeping related shots consistent: a related-shot group chosen by the user
    // (items with the same group form exactly that group; analysis::group_related_shots), and
    // whether the photo is an anchor of its group (its current settings set the group's look and are
    // kept). Ignored otherwise. A group name is 1..64 bytes of UTF-8.
    std::optional<std::string> group;
    bool anchor = false;
};

struct BatchFolder {
    std::filesystem::path path;  // imported (with BatchServiceOptions::registry) before selection
    bool include_subfolders = true;
};

// Automatic settings of a batch (see "Automatic settings" above).
enum class AutomaticSettings : std::uint8_t {
    Off = 0,          // export the recipe frozen at submission
    Auto = 1,         // analyse, freeze as an automatic revision, check at full resolution, export
    AnalyzeOnly = 2,  // analyse and freeze as automatic revisions; export nothing
};

[[nodiscard]] std::string_view to_string(AutomaticSettings value) noexcept;  // "off", "auto", "analyze_only"

// What happens to a job whose identical output (same identity) is already in the destination.
enum class ExistingOutputs : std::uint8_t {
    Skip = 1,         // skipped as "already exported", pointing at the existing file
    ExportAgain = 2,  // exported again under a new name (the single-item export's behaviour)
};

struct BatchRequest {
    std::string name;  // shown in lists (UTF-8); empty: "Export of <n> photos"
    std::vector<BatchItem> items;
    std::optional<BatchFolder> folder;  // added to `items`
    BatchDestination destination;
    exporter::ExportSettings output = exporter::ExportSettings::jpeg_srgb_q95();
    // The user's rights text written as Artist / Copyright (MetadataPolicy::include_copyright).
    // std::nullopt: the source's own values; an empty string: none.
    std::optional<std::string> artist;
    std::optional<std::string> copyright;
    ExistingOutputs existing_outputs = ExistingOutputs::Skip;
    // Export Unsupported photographs anyway (blueprint section 3: never silently; the report and
    // the receipt say so and recommend a review).
    bool allow_unsupported = false;
    bool start = true;               // false: a Draft batch, started with start()
    bool write_report_files = true;  // the report files in the output folder when it finishes
    // An interactive single export (ExportService): dispatched before other batches' jobs and
    // shown as one photo rather than as a batch (BatchStatus::single_export).
    bool single_export = false;
    // Automatic settings (see "Automatic settings" above) and whether related photographs are kept
    // visually consistent (needs Auto or AnalyzeOnly; InvalidArgument otherwise).
    AutomaticSettings automatic = AutomaticSettings::Off;
    bool keep_consistent = false;
};

// ---------------------------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kPlanExamples = 3;
inline constexpr std::size_t kMaxListedExclusions = 1000;

struct PlannedOutput {
    ItemId item;
    std::filesystem::path source;
    // The path the output gets if the folder does not change before it is published; for an
    // output already exported, the existing file.
    std::filesystem::path destination;
    bool renamed = false;            // a "_n" suffix was needed (inside the batch or in the folder)
    bool replaces_existing = false;  // CollisionPolicy::Overwrite will replace an existing file
    bool already_exported = false;   // ExistingOutputs::Skip: not rendered; `destination` exists
};

// Facts about a plan the user interface words itself (stable codes; BatchPlan::warnings has the
// developer-facing English).
enum class PlanNotice : std::uint8_t {
    NetworkDestination = 1,    // keep the connection up; files are verified when written
    RemovableDestination = 2,  // keep the drive connected until the batch finishes
    ReplacesExisting = 3,      // CollisionPolicy::Overwrite (never an original, never an own output)
    WritesGps = 4,             // GPS positions are written into the outputs
    MayRunOutOfSpace = 5,      // free_bytes < estimated_output_bytes (an upper bound)
    TooFullForOne = 6,         // free_bytes < largest_output_bytes + reserve: submit() refuses
    // Every photo is analysed first (a few seconds each, about as long as its export); outputs are
    // named after the automatic revisions, so example names may show an earlier revision number.
    AutomaticSettings = 7,
    AnalyzeOnly = 8,       // nothing is exported: automatic revisions and a report only
    KeepConsistent = 9,    // related photographs are analysed together first (a second analysis each)
};

struct PlanExclusion {
    ItemId item;
    std::filesystem::path source;  // empty when the item is unknown
    Error reason;
};

struct BatchPlan {
    std::filesystem::path folder;  // canonical when it exists
    bool folder_exists = false;
    core::StorageKind storage = core::StorageKind::Unknown;
    // kPlanExamples outputs: the first ones that will be written, then (when fewer will be
    // written) the first already exported ones.
    std::vector<PlannedOutput> examples;
    std::uint64_t outputs = 0;            // photographs that will be rendered
    std::uint64_t to_hash = 0;            // sources submit() hashes first
    std::uint64_t already_exported = 0;   // identical outputs found (skipped under ExistingOutputs::Skip)
    std::uint64_t renamed = 0;
    std::uint64_t replaced = 0;
    std::vector<PlanExclusion> excluded;  // at most kMaxListedExclusions
    std::uint64_t excluded_count = 0;
    std::uint64_t estimated_output_bytes = 0;  // upper bound: the uncompressed size of every output
    std::uint64_t largest_output_bytes = 0;
    std::optional<std::uint64_t> free_bytes;  // of the destination volume (when it exists)
    std::vector<PlanNotice> notices;          // in PlanNotice order
    std::vector<std::string> warnings;        // developer-facing English (network destination, ...)
};

struct SubmittedBatch {
    catalog::BatchId id;
    BatchPlan plan;
    std::uint64_t jobs = 0;             // created (including those skipped as already exported)
    std::uint64_t skipped_existing = 0;
};

struct BatchSubmitProgress {
    enum class Phase : std::uint8_t { Importing = 1, Hashing = 2, Planning = 3, Writing = 4 };
    Phase phase = Phase::Planning;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
};
using BatchSubmitProgressCallback = std::function<void(const BatchSubmitProgress&)>;

// ---------------------------------------------------------------------------------------------
// Status and events
// ---------------------------------------------------------------------------------------------

// Why the service paused a batch by itself. Held in memory only: after a restart the batch is just
// Paused (the job it gave back records no failure), and resume() checks the destination again,
// raising the attention again while the problem lasts.
enum class BatchAttention : std::uint8_t {
    None = 0,
    DestinationFull = 1,         // free space below what the next output needs
    DestinationUnavailable = 2,  // the folder or its drive is gone (resume() once it is back)
    DestinationPermission = 3,   // files can no longer be created there
    // Another folder object is at the output folder's path (it was deleted and created again, or
    // another drive got its letter): use_replaced_folder() continues there, resume() checks again.
    DestinationReplaced = 4,
};

[[nodiscard]] std::string_view to_string(BatchAttention attention) noexcept;

struct RunningJob {
    catalog::JobId job;
    std::optional<ItemId> item;
    std::uint32_t ordinal = 0;
    std::uint32_t attempt = 0;
    std::filesystem::path source;
    workers::JobStage stage = workers::JobStage::Preparing;
    double fraction = 0.0;
    std::uint32_t slot = 0;
};

struct BatchStatus {
    catalog::BatchRecord record;  // state, counts, timestamps, documents
    std::filesystem::path folder;
    BatchAttention attention = BatchAttention::None;
    std::optional<Error> attention_error;  // developer-facing
    // DestinationFull: the free space seen and what the next output may need (bytes).
    std::optional<std::uint64_t> attention_free_bytes;
    std::uint64_t attention_needed_bytes = 0;
    std::vector<RunningJob> running;
    // Remaining time at the measured pace of this session, once three jobs have completed; only
    // while the batch runs without an attention.
    std::optional<double> eta_seconds;
    bool single_export = false;            // BatchRequest::single_export
    std::uint64_t excluded = 0;            // photographs left out when the batch was created
    // Exported outputs whose receipt recommends a review, plus analysed photos (AnalyzeOnly) whose
    // automatic revision's record does.
    std::uint64_t review_recommended = 0;
    std::uint64_t retryable = 0;           // failed jobs retry_failed() requeues
    catalog::AutoMode automatic = catalog::AutoMode::None;
    bool keep_consistent = false;
    // Automatic batches: their unfinished jobs by automatic state (the pre-pass progresses while
    // pending falls).
    std::optional<catalog::AutoJobCounts> automatic_counts;
    // A batch of one photograph: its output once exported (or the existing file it was skipped
    // for), and why it was not exported (failed, skipped or cancelled with a reason).
    std::optional<std::filesystem::path> output;
    std::optional<Error> outcome_error;
    std::string outcome_reason;  // user-facing English for outcome_error (as ReportEntry::reason)
};

enum class BatchEventKind : std::uint8_t {
    Created = 1,       // submit() wrote the batch
    StateChanged = 2,  // start, pause, resume, cancel, retry, completion
    JobStarted = 3,    // a slot began an attempt
    JobProgress = 4,   // the worker's progress (about every 1 %)
    JobFinished = 5,   // the attempt ended: complete, failed, cancelled, requeued or skipped
    Attention = 6,     // the service paused the batch (BatchAttention)
    Finished = 7,      // every job is terminal (Completed, or Cancelled with nothing running); the
                       // report files, when the batch writes them, are already in place
    ReportWritten = 8,
};

struct BatchEvent {
    BatchEventKind kind = BatchEventKind::StateChanged;
    catalog::BatchId batch;
    catalog::BatchState state = catalog::BatchState::Running;
    catalog::BatchCounts counts;
    std::optional<catalog::JobId> job;
    std::optional<ItemId> item;
    catalog::JobStage job_stage = catalog::JobStage::Queued;  // JobFinished: the stage it ended in
    workers::JobStage worker_stage = workers::JobStage::Preparing;
    double fraction = 0.0;
    BatchAttention attention = BatchAttention::None;
    // Attention (DestinationFull): the free space seen and what the next output may need.
    std::optional<std::uint64_t> attention_free_bytes;
    std::uint64_t attention_needed_bytes = 0;
    std::optional<Error> error;                   // JobFinished / Attention: why
    std::optional<std::filesystem::path> output;  // JobFinished (Complete) / ReportWritten
};

using BatchObserver = std::function<void(const BatchEvent&)>;

// ---------------------------------------------------------------------------------------------
// Reports and receipts
// ---------------------------------------------------------------------------------------------

// The details a receipt records besides path, size and hash ("lucent.batch-receipt" v1 JSON,
// catalog::ReceiptRecord::settings_json).
struct ReceiptDetails {
    std::optional<catalog::RevisionId> revision;
    std::uint32_t revision_number = 0;
    core::Digest256 recipe_sha256{};
    core::Digest256 export_settings_sha256{};
    exporter::ExportSettings settings;
    exporter::ExportFormat format = exporter::ExportFormat::Jpeg;
    exporter::OutputProfile profile = exporter::OutputProfile::Srgb;
    exporter::BitDepth bit_depth = exporter::BitDepth::Eight;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    core::Digest256 icc_sha256{};
    std::optional<exporter::ChromaSubsampling> chroma_subsampling;
    std::string encoder;
    std::vector<std::string> metadata_notes;
    raw::InterpretationStatus interpretation = raw::InterpretationStatus::Complete;
    std::vector<std::string> warnings;
    std::string demosaic;
    std::string camera_color;
    std::uint32_t pipeline_version = 0;
    std::string look;
    std::uint32_t look_version = 0;
    std::uint64_t clipped_raw_samples = 0;
    std::uint64_t limited_display_pixels = 0;
    std::uint64_t estimated_peak_bytes = 0;
    std::uint64_t worker_peak_commit_bytes = 0;
    double prepare_seconds = 0.0;
    double render_seconds = 0.0;
    double encode_seconds = 0.0;
    double write_seconds = 0.0;
    double verify_seconds = 0.0;  // the worker's re-hash and the coordinator's verification
    double total_seconds = 0.0;   // the attempt, dispatch to receipt
    std::uint32_t attempt = 0;
    std::uint32_t name_index = 1;  // 1: the pattern's name; n: "_n" was appended
    bool replaced_existing = false;
    bool allow_unsupported = false;
    std::vector<std::string> review;
    // W10 (ADR 0018): the photo's support status when it was exported and the camera support
    // database version it was assessed with (empty: none). std::nullopt in receipts written before.
    std::optional<SupportStatus> support;
    std::string camera_support_version;
    // W13 (ADR 0023): the lens database the worker rendered with (app::lens_database_text(); empty:
    // none) and the lens corrections it applied (app::lens_corrections_text(); empty: none). Empty in
    // receipts written before.
    std::string lens_database;
    std::string lens_corrections;
};

enum class ReportOutcome : std::uint8_t {
    Exported = 1,
    Skipped = 2,  // already exported, or excluded at submission
    Failed = 3,
    Cancelled = 4,
    Pending = 5,   // queued or running (a report of an unfinished batch)
    Analysed = 6,  // AnalyzeOnly: automatic revisions written, nothing exported
};

[[nodiscard]] std::string_view to_string(ReportOutcome outcome) noexcept;

// What automatic settings did to one photo (automatic batches), from its effective revision's
// analysis record.
struct AutomaticReport {
    catalog::AutoJobState state = catalog::AutoJobState::None;
    std::optional<catalog::RevisionId> revision;  // the effective automatic revision (once frozen)
    std::uint32_t revision_number = 0;
    double confidence = 0.0;
    std::uint32_t attempt = 0;     // the effective attempt (0: the optimiser's choice; n: a fallback)
    std::uint32_t attempts = 0;    // automatic revisions written (failed attempts included)
    bool review = false;           // the record recommends a review
    std::vector<std::string> flags;  // user-facing English, one per flag
    std::optional<std::string> group;    // the related-shot group it was regularised in (or, for an anchor, set)
    bool locks_prevented_targets = false;
    std::vector<std::string> locks;      // user-facing English: what the locks did
    // The photo was edited while the batch ran: its automatic revisions are a branch of its history,
    // not its current recipe (recipe_store.hpp, kAutomaticBranchNote).
    bool branch = false;
    // A caller-marked related-shot anchor: its own settings were kept (AutoAttemptKind::Anchor).
    bool anchor = false;
};

struct ReportEntry {
    std::optional<catalog::JobId> job;  // std::nullopt for an exclusion
    std::optional<ItemId> item;
    std::uint32_t ordinal = 0;
    std::filesystem::path source;
    ReportOutcome outcome = ReportOutcome::Pending;
    std::optional<std::filesystem::path> output;
    std::uint64_t size_bytes = 0;
    std::optional<core::Digest256> sha256;
    std::optional<Error> error;   // failure or skip reason (developer-facing message)
    std::string reason;           // user-facing English: what happened
    std::string action;           // user-facing English: what to do (empty: nothing)
    std::vector<std::string> review;  // why a review is recommended (empty: not recommended)
    // W13 (ADR 0023): the lens database and corrections of an exported photo (its receipt's).
    std::string lens_database;
    std::string lens_corrections;
    // W10 (ADR 0018): the photo's support status as the batch knew it (its receipt for exported
    // photos; Unsupported for photos skipped as unsupported); std::nullopt when not recorded.
    std::optional<SupportStatus> support;
    std::uint32_t attempts = 0;
    std::optional<ReceiptDetails> details;  // Exported: what the receipt records
    std::optional<AutomaticReport> automatic;  // automatic batches
};

struct BatchReport {
    catalog::BatchRecord batch;
    std::filesystem::path folder;
    std::uint64_t exported = 0;
    std::uint64_t skipped = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t pending = 0;
    std::uint64_t analysed = 0;            // AnalyzeOnly: photos with automatic revisions
    std::uint64_t review_recommended = 0;  // exported (or analysed) photos with review notes
    std::vector<ReportEntry> entries;      // jobs in batch order, then exclusions
    std::vector<std::string> notes;        // batch-level notes (weaker durability of the destination, ...)
    catalog::Timestamp generated_at{};
};

struct ReportFiles {
    std::filesystem::path json;
    std::filesystem::path html;
};

// "lucent.batch-report" v1 JSON and the readable HTML page of `report`.
[[nodiscard]] Expected<std::string> report_json(const BatchReport& report);
[[nodiscard]] std::string report_html(const BatchReport& report);

[[nodiscard]] Expected<std::string> write_receipt_details(const ReceiptDetails& details);
// Corrupt ("app.batch.receipt") for a document that is not "lucent.batch-receipt" v1;
// VersionMismatch for a newer version.
[[nodiscard]] Expected<ReceiptDetails> read_receipt_details(std::string_view json);

// ---------------------------------------------------------------------------------------------
// Options and statistics
// ---------------------------------------------------------------------------------------------

struct BatchMemoryOptions {
    // Aggregate Lucent budget (UI, caches, workers); 0: half the physical memory, clamped to
    // [2 GiB, 8 GiB] (8 GiB on the 16 GB reference laptop).
    std::uint64_t total_bytes = 0;
    // Kept for Lucent.exe's interactive work (previews, edit sessions, caches).
    std::uint64_t interactive_reserve_bytes = std::uint64_t{3} << 29;  // 1.5 GiB
    // A worker process's commit before its first job (DLLs, LibRaw and codec state).
    std::uint64_t worker_baseline_bytes = std::uint64_t{160} << 20;
    // Estimated commit per output pixel until a job has been measured. The engine estimates about
    // 24 bytes per pixel for an export render (1 GiB for 45 MP); the W7 measurement of the 45 MP
    // fixture's JPEG export was 731 MiB of peak commit.
    double default_bytes_per_pixel = 24.0;
    // The same for attempts that run an automatic analysis (automatic batches): the full-resolution
    // final check holds the analysis' stage one, its saturation classes and one stage two, and
    // measures by bands (app::estimate_automatic_memory: about 37 bytes per pixel since W12, 60-70
    // before; the R5's analysis attempt peaks at about 24 measured).
    double default_auto_bytes_per_pixel = 40.0;
    // Margin of measured estimates (1.15: 15 % above the largest measured job).
    double measured_margin = 1.15;
    // A further worker is admitted only while the system's available physical memory exceeds its
    // estimate by this much.
    std::uint64_t free_memory_margin_bytes = std::uint64_t{512} << 20;
};

struct BatchRetryPolicy {
    std::uint32_t max_transient_failures = 3;  // the job fails at the next transient failure
    std::chrono::milliseconds initial_backoff{2'000};
    std::chrono::milliseconds max_backoff{60'000};
    // Interruptions (Lucent stopped, or the lease expired) a job may have; the next one fails it
    // ("Lucent was interrupted repeatedly while exporting this photo").
    std::uint32_t max_interruptions = 5;
};

// Test-only seams. Never set in production.
struct BatchTestHooks {
    // Worker test hooks (workers/worker_launch.hpp) for the n-th worker process the service starts
    // (any slot, counted from 0); later processes get none.
    std::vector<std::vector<std::string>> worker_hooks_by_start;
    // Called on the slot thread at the named points of the publication protocol: "reserved",
    // "temp_written", "verified", "before_rename", "after_rename", "published", "receipt",
    // "complete"; and of automatic jobs: "answered" (the worker's analysis arrived; nothing of it is
    // written yet), "analysed" (the pre-pass recorded a job's analysis), "frozen". A test coordinator
    // process ends itself here to simulate a crash.
    std::function<void(std::string_view point, catalog::JobId job)> on_point;
    // Replaces the free-space query of destination folders (bytes available; std::nullopt:
    // unknown).
    std::function<std::optional<std::uint64_t>(const std::filesystem::path& directory)> free_space;
    // A further worker is admitted before any job was measured (its estimates still have to fit),
    // so tests can run two workers at once from the first jobs.
    bool admit_before_calibration = false;
};

struct BatchServiceOptions {
    std::filesystem::path worker_executable;  // empty: workers::default_worker_executable()
    std::uint32_t max_workers = 2;            // 1..4 worker slots
    BatchMemoryOptions memory;
    BatchRetryPolicy retry;
    std::chrono::milliseconds lease_duration{60'000};
    std::chrono::milliseconds lease_renewal{10'000};
    std::chrono::milliseconds job_timeout{15 * 60'000};  // one attempt, dispatch to answer
    std::chrono::milliseconds heartbeat_interval{500};   // worker heartbeats
    std::chrono::milliseconds heartbeat_timeout{15'000};
    std::chrono::milliseconds cancel_grace{5'000};
    // How often leases nobody renews are reconciled, and reconciliations that waited for an
    // unreachable output folder are tried again.
    std::chrono::milliseconds reclaim_interval{30'000};
    // True while interactive editing waits for a render (no new job is admitted meanwhile).
    // Called on the coordinator thread; must be cheap and thread-safe.
    std::function<bool()> interactive_busy;
    // For folder batches (imports); without it a BatchRequest::folder is refused.
    std::shared_ptr<const raw::DecoderRegistry> registry;
    std::vector<std::filesystem::path> lucent_directories;  // never imported (folder batches)
    // W10 (ADR 0018): the camera support database the photos a folder batch imports are assessed
    // against (null: none).
    CameraSupportRef camera_support;
    // W13 (ADR 0023): the lens database this session renders with (lens_database_text(): asset id,
    // version and manifest SHA-256; empty: none). It is part of every new batch's renderer_version,
    // and so of every job's identity (blueprint section 7: "renderer and asset versions"): an output
    // made with another database is not taken for this one's.
    std::string lens_database;
    // W12 (APL-06): when set, the lens database's text is this function's result instead (empty: none).
    // It is asked for once, when the first batch is planned or submitted (never by open()), and kept. It
    // may block: Lucent.exe's waits for its lens database, which loads off the UI thread
    // (lens_database_source.hpp), so a batch never names another database than the one it renders with.
    std::function<std::string()> lens_database_source;
    // Exporter temporaries ("~lucent-<hex>.tmp") older than this are removed by the leftover sweep.
    std::chrono::seconds stale_temporary_age{3600};
    std::function<catalog::Timestamp()> clock;  // empty: catalog::current_time
    BatchTestHooks test;

    // InvalidArgument ("app.batch.options") for values outside the documented ranges.
    [[nodiscard]] Status validate() const;
};

struct RecoveryReport {
    std::uint64_t reclaimed = 0;        // leases of a previous session
    std::uint64_t completed = 0;        // published outputs verified and completed
    std::uint64_t requeued = 0;
    std::uint64_t failed = 0;           // receipts whose output is gone or changed
    std::uint64_t orphans_removed = 0;  // outputs renamed into place without a Published step
    std::uint64_t publications_closed = 0;  // journal attempts marked Abandoned
    std::uint64_t leftovers_removed = 0;    // temporary files and scratch folders
    std::uint64_t restored_aside = 0;       // files an interrupted overwrite had moved aside
    // Leases and publications left for later because the output folder was out of reach or an
    // output could not be read (reconciled every reclaim_interval once it is back).
    std::uint64_t deferred = 0;
    std::uint64_t reports_written = 0;  // reports of batches that finished just before a stop
};

struct BatchSchedulerStats {
    std::uint64_t total_budget_bytes = 0;
    std::uint64_t worker_budget_bytes = 0;
    std::uint64_t interactive_reserve_bytes = 0;
    std::uint32_t max_workers = 0;
    std::uint32_t active_workers = 0;
    std::uint32_t peak_active_workers = 0;
    bool calibrated = false;          // a job's peak commit has been measured
    double bytes_per_pixel = 0.0;     // used by the estimates now
    std::uint64_t peak_worker_commit_bytes = 0;
    std::uint64_t worker_starts = 0;  // processes started (restarts included)
    std::uint64_t attempts = 0;
    std::uint64_t completed = 0;
    std::uint64_t retried = 0;
    std::uint64_t failed = 0;
    std::uint64_t deferred_for_memory = 0;       // admissions postponed by the budget
    std::uint64_t deferred_for_interactive = 0;  // ... by interactive work
    std::vector<std::uint32_t> worker_pids;      // running worker processes
    // The hard aggregate limit of the workers' commit together (the worker budget, enforced by a
    // workers::WorkerGroup); 0 when it could not be set up (per-process limits only).
    std::uint64_t aggregate_limit_bytes = 0;
    // The workers' largest total commit (the group's Job Object accounting; it includes the charge
    // of an allocation the limit refused).
    std::uint64_t peak_aggregate_commit_bytes = 0;
    std::uint64_t aggregate_limit_hits = 0;   // attempts the limit stopped beside other workers
    std::uint64_t exclusive_attempts = 0;     // attempts then admitted alone
    std::uint64_t idle_workers_released = 0;  // idle workers shut down to make room under the limit
};

// ---------------------------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------------------------

class BatchService {
public:
    // Starts the coordinator, which first reconciles the journal (see "Recovery"). Errors
    // ("app.batch.open"): InvalidArgument for invalid options, a null catalog, or a library that is
    // not persistent (the journal references catalog items); PermissionDenied / VersionMismatch
    // for a read-only catalog; the catalog's errors. `library`, `recipes` and the catalog must
    // outlive the service.
    [[nodiscard]] static Expected<std::unique_ptr<BatchService>> open(Library& library,
                                                                      std::shared_ptr<catalog::Catalog> catalog,
                                                                      RecipeService& recipes,
                                                                      BatchServiceOptions options = {});

    ~BatchService();
    BatchService(const BatchService&) = delete;
    BatchService& operator=(const BatchService&) = delete;

    // See "Creating a batch". Errors ("app.batch.*"): InvalidArgument (destination, pattern,
    // settings, empty selection: "app.batch.empty" when nothing can be exported); NotFound /
    // PermissionDenied / DiskFull / IoError for the destination; Cancelled; the catalog's.
    [[nodiscard]] Expected<BatchPlan> plan(const BatchRequest& request,
                                           const core::CancellationToken& cancel = {}) const;
    [[nodiscard]] Expected<SubmittedBatch> submit(const BatchRequest& request,
                                                  const BatchSubmitProgressCallback& progress = {},
                                                  const core::CancellationToken& cancel = {});

    // Batch commands (catalog/catalog.hpp semantics; repeating one is a no-op). NotFound for an
    // unknown batch; "catalog.batch.invalid_transition" otherwise.
    [[nodiscard]] Status start(catalog::BatchId batch);
    [[nodiscard]] Status pause(catalog::BatchId batch);
    [[nodiscard]] Status resume(catalog::BatchId batch);  // also clears a BatchAttention
    // Accepts the folder object now at the output folder's path as the batch's output folder (after
    // BatchAttention::DestinationReplaced: the user created it again, or wants another drive's
    // folder of that name), records it in the batch and resumes. NotFound when no folder is there.
    [[nodiscard]] Status use_replaced_folder(catalog::BatchId batch);
    // With `reason`, the jobs it cancels record it (reports then say why, for example the full
    // disk that made a single export give up).
    [[nodiscard]] Status cancel(catalog::BatchId batch, const std::optional<Error>& reason = std::nullopt);
    // Queues the batch's failed jobs again; returns how many. A job whose published output was
    // lost after its receipt committed stays failed (export the photo again).
    [[nodiscard]] Expected<std::uint64_t> retry_failed(catalog::BatchId batch);

    [[nodiscard]] Expected<BatchStatus> status(catalog::BatchId batch) const;
    // Newest first: at most `limit` batches with ids below `before` (0: from the newest).
    [[nodiscard]] Expected<std::vector<BatchStatus>> list(std::uint32_t limit = 50, catalog::BatchId before = {}) const;
    // Every batch with work left (Draft, Running, Paused, or Cancelled with a job still running),
    // whatever its age, newest first.
    [[nodiscard]] Expected<std::vector<BatchStatus>> list_unfinished() const;

    [[nodiscard]] Expected<BatchReport> report(catalog::BatchId batch) const;
    // Writes "Lucent-batch-<id>-report.json" and ".html" into the batch's output folder (replacing
    // an earlier report of the same batch).
    [[nodiscard]] Expected<ReportFiles> write_report(catalog::BatchId batch);
    // The folder "Open output folder" opens.
    [[nodiscard]] Expected<std::filesystem::path> output_folder(catalog::BatchId batch) const;

    [[nodiscard]] Subscription subscribe(BatchObserver observer);

    [[nodiscard]] BatchSchedulerStats stats() const;
    // What the reconciliation at open did (complete once wait_for_recovery() returned true).
    [[nodiscard]] RecoveryReport last_recovery() const;
    // Waits until the reconciliation at open has run; false after `timeout`.
    [[nodiscard]] bool wait_for_recovery(std::chrono::milliseconds timeout) const;
    // Re-evaluates admission now (for example when interactive work became idle).
    void wake() noexcept;

    struct Impl;  // implementation detail

private:
    explicit BatchService(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lucent::app
