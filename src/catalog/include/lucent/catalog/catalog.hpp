#pragma once

// The Lucent catalog: a local SQLite database of sources, items, user flags, import records, the
// recipe revisions of items and the durable batch journal (blueprint sections 2, 7 and 8). SQLite is a private dependency; no SQLite
// type or header appears here.
//
// Storage and durability
//   * One database file (default %LOCALAPPDATA%\Lucent\Catalog\lucent.catalog, location.hpp) on a
//     local disk; network locations are refused (check_catalog_storage).
//   * Write-ahead logging (journal_mode=WAL): readers never block the writer and see a consistent
//     snapshot; the writer never blocks readers.
//   * synchronous=NORMAL by default (Durability::Normal). In WAL mode a commit is atomic and
//     survives a crash of Lucent at any point. After a power failure or operating-system crash
//     the database is still consistent, but the most recent commits (those since the last WAL
//     checkpoint) may be rolled back. Durability::Full (synchronous=FULL) syncs the WAL on every
//     commit and also keeps those, at the cost of one flush per transaction.
//   * foreign_keys=ON, STRICT tables, defensive mode, no double-quoted string literals; every
//     value is bound as a statement parameter, never spliced into SQL text.
//   * busy_timeout (OpenOptions::busy_timeout) applies when another process holds a lock.
//
// Concurrency: one writer, pooled readers
//   * All writes go through one dedicated writer connection, serialised by a mutex: write() runs
//     its callback inside BEGIN IMMEDIATE ... COMMIT while holding it, so a batch of many changes
//     (an import of 1,000 files) is one transaction and one commit, not one per row.
//   * read() leases one of up to OpenOptions::max_readers read-only connections and runs its
//     callback inside one read transaction (a stable WAL snapshot of the last commit).
//   * A ReadWrite catalog holds an exclusive lock file ("<catalog>.lock", deleted on close) for
//     its lifetime, so a second ReadWrite open of the same catalog, from this or another process,
//     fails instead of becoming a second uncoordinated writer, and restore() refuses a catalog
//     that is open.
//
// Schema versions and migrations (blueprint section 8)
//   * PRAGMA application_id marks a Lucent catalog; PRAGMA user_version is its schema version
//     (kSchemaVersion is the version this build writes); the catalog_meta table records the
//     schema version, the oldest schema reader that can still read it, and the application
//     versions that created and last migrated it; schema_history lists every applied migration.
//   * An empty file is version 0 and is initialised by the ordered migrations 0 -> 1 -> ...
//   * An older catalog is first copied with SQLite's online backup API into the backup folder
//     ("pre-migration" backup), the copy is verified (integrity_check, application id and schema
//     version), and only then are the migrations applied, each in its own transaction (with
//     foreign-key verification before commit). A failed migration rolls back to the last
//     completed version and open() fails; the pre-migration backup remains.
//   * A newer catalog (written by a later Lucent) is never written and never migrated.
//     NewerSchemaPolicy::OpenReadOnly (the default) opens it read-only when the newer schema
//     declares that this version can still read it (its minimum reader version), and records the
//     VersionMismatch in SchemaStatus::read_only_reason; every write then fails with that error.
//     When the newer schema is not readable by this build, or under NewerSchemaPolicy::Refuse,
//     open() fails with VersionMismatch.
//   * A SQLite database that is not a Lucent catalog is refused (Unsupported) and never touched.
//
// Integrity: open() runs PRAGMA quick_check (or the full integrity_check plus foreign_key_check,
// OpenOptions::integrity_check) before using or migrating a catalog. A damaged catalog fails with
// ErrorCode::Corrupt; nothing is written to it. The user can restore() a backup.
//
// Errors use the stable locations "catalog.open", "catalog.migrate", "catalog.read",
// "catalog.write", "catalog.backup", "catalog.restore" and "catalog.<operation>" for repository
// calls. SQLite results map to ErrorCode as follows (the extended SQLite result code is kept in
// Error::native_code): BUSY/LOCKED -> IoError (transient); CORRUPT/NOTADB -> Corrupt;
// FULL -> DiskFull; NOMEM -> OutOfMemory; READONLY/PERM/AUTH -> PermissionDenied;
// CONSTRAINT_UNIQUE/PRIMARYKEY -> AlreadyExists; CONSTRAINT_FOREIGNKEY -> NotFound; other
// constraints -> InvalidArgument; TOOBIG -> LimitExceeded; INTERRUPT -> Cancelled; misuse and SQL
// errors -> Internal; anything else -> IoError.
//
// Thread-safety: every member of Catalog may be called concurrently from any threads, except
// that the object must not be destroyed while calls are in progress. A ReadTransaction or
// WriteTransaction is valid only inside the callback it was passed to, on that thread.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "lucent/catalog/jobs.hpp"
#include "lucent/catalog/location.hpp"
#include "lucent/catalog/types.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/file.hpp"
#include "lucent/core/hash.hpp"

namespace lucent::catalog {

// Schema version this build creates and writes.
//   1 (W4): catalog_meta, schema_history, imports, sources, items.
//   2 (W6): recipe_revisions, items.current_revision_id and the capture fields of items (ISO,
//           exposure time, f-number, focal length, image count). Adds only, but edits change
//           what renders mean, so its minimum reader version is 2: a schema-v1 build refuses a
//           v2 catalog instead of showing and exporting edited photos as shot.
//   3 (W7): the durable batch journal: batches, jobs, job_events, publication_journal and
//           receipts (jobs.hpp). Adds only and does not change what renders of items mean, so
//           its minimum reader version stays 2: a schema-v2 build opens a v3 catalog read-only.
inline constexpr std::int32_t kSchemaVersion = 3;

enum class OpenMode : std::uint8_t {
    ReadWrite = 1,  // the application's catalog: migrates, writes, holds the writer lock
    ReadOnly = 2,   // inspection (diagnostics, a backup): never writes; every write fails
};

enum class NewerSchemaPolicy : std::uint8_t {
    OpenReadOnly = 1,  // open a readable newer catalog read-only (see the file comment)
    Refuse = 2,        // fail with VersionMismatch
};

enum class IntegrityCheck : std::uint8_t {
    None = 0,   // skip (tests and tools only)
    Quick = 1,  // PRAGMA quick_check: page and record structure, O(database size)
    Full = 2,   // PRAGMA integrity_check (adds index consistency) and foreign_key_check
};

enum class Durability : std::uint8_t {
    Normal = 1,  // synchronous=NORMAL (see the file comment)
    Full = 2,    // synchronous=FULL
};

enum class BackupKind : std::uint8_t {
    Manual = 1,        // Catalog::create_backup()
    PreMigration = 2,  // taken by open() before migrating an older catalog
    PreRestore = 3,    // taken by restore() before replacing the catalog
};

// "manual", "pre-migration", "pre-restore" (also the token in backup file names).
[[nodiscard]] std::string_view to_string(BackupKind kind) noexcept;

struct OpenOptions {
    std::filesystem::path path;  // the catalog file; absolute or relative to the current directory
    OpenMode mode = OpenMode::ReadWrite;
    // ReadWrite only: create the file (and its folder) when it does not exist. Otherwise a missing
    // file is NotFound.
    bool create_if_missing = true;
    NewerSchemaPolicy newer_schema = NewerSchemaPolicy::OpenReadOnly;
    IntegrityCheck integrity_check = IntegrityCheck::Quick;
    Durability durability = Durability::Normal;
    std::chrono::milliseconds busy_timeout{5000};  // 0 ... 10 minutes
    std::uint32_t max_readers = 4;                 // pooled read connections, 1 ... 64
    // Folder for automatic and manual backups; empty: default_backup_directory(path).
    std::filesystem::path backup_directory;
    // Backups of each kind kept in the backup folder; older ones of that kind are deleted after a
    // new one is written. 1 ... 1000.
    std::uint32_t backups_to_keep = 5;
    // Application version recorded in catalog_meta and schema_history (SemVer text); empty:
    // core::build_info().version.
    std::string app_version;
};

struct BackupInfo {
    std::filesystem::path path;
    BackupKind kind = BackupKind::Manual;
    Timestamp created_at{};
    std::int32_t schema_version = 0;
    std::uint64_t size_bytes = 0;
};

struct SchemaStatus {
    std::int32_t found_version = 0;  // schema version of the file before open() (0: empty)
    std::int32_t version = 0;        // schema version now
    std::int32_t supported_version = kSchemaVersion;
    bool created = false;                          // the file was empty and has been initialised
    std::vector<std::int32_t> migrations_applied;  // versions reached by open(), in order
    std::optional<BackupInfo> pre_migration_backup;
    std::string created_by;  // application version that created the catalog ("" if unknown)
    std::string written_by;  // application version that last migrated it ("" if unknown)
    // Why a ReadWrite open produced a read-only catalog (VersionMismatch for a newer schema).
    std::optional<Error> read_only_reason;
};

struct RestoreOptions {
    // Folder for the pre-restore backup of the current catalog; empty: the default folder.
    std::filesystem::path backup_directory;
    std::uint32_t backups_to_keep = 5;
    std::chrono::milliseconds busy_timeout{5000};
};

struct RestoreReport {
    std::int32_t restored_schema_version = 0;
    // Online backup of the catalog that was replaced (std::nullopt when there was none or it was
    // unreadable).
    std::optional<BackupInfo> pre_restore_backup;
    // When the replaced catalog could not be backed up (for example because it is corrupt), its
    // files were moved aside under these names instead of being deleted.
    std::vector<std::filesystem::path> moved_aside;
};

class Catalog;

namespace detail {
class Connection;
struct CatalogAccess;
}  // namespace detail

// Queries, valid inside Catalog::read() and Catalog::write() callbacks. Every result reflects one
// consistent state: the snapshot of the read transaction, or the writer's own uncommitted
// changes inside write().
class ReadTransaction {
public:
    ReadTransaction(const ReadTransaction&) = delete;
    ReadTransaction& operator=(const ReadTransaction&) = delete;

    // Sources. "find" functions return std::nullopt when nothing matches.
    [[nodiscard]] Expected<std::optional<SourceRecord>> find_source(SourceId id) const;
    // Looks up make_path_key(path); InvalidArgument for a path make_path_key rejects.
    [[nodiscard]] Expected<std::optional<SourceRecord>> find_source_by_path(
        const std::filesystem::path& path) const;
    // Sources whose catalogued file object is (volume_serial, file_id), in id order: other paths
    // to the same file (hard links), or a source whose file was moved within its volume.
    [[nodiscard]] Expected<std::vector<SourceRecord>> find_sources_by_file_id(
        std::uint64_t volume_serial, const std::array<std::uint8_t, 16>& file_id) const;
    // Sources with this content hash, in id order (content duplicates).
    [[nodiscard]] Expected<std::vector<SourceRecord>> find_sources_by_sha256(
        const core::Digest256& sha256) const;
    // Sources in id order (refresh scans, relink candidates). InvalidArgument for a bad limit.
    [[nodiscard]] Expected<std::vector<SourceRecord>> list_sources(const SourceQuery& query) const;
    [[nodiscard]] Expected<std::uint64_t> count_sources() const;

    // Items.
    [[nodiscard]] Expected<std::optional<ItemRecord>> find_item(ItemId id) const;
    // Items of one source in subimage order (empty when the source has none or does not exist).
    [[nodiscard]] Expected<std::vector<ItemRecord>> items_of_source(SourceId source) const;
    // One page of the contact sheet. InvalidArgument for a bad limit or min_rating, or a cursor
    // of another order.
    [[nodiscard]] Expected<ItemPage> list_items(const ItemQuery& query) const;
    [[nodiscard]] Expected<std::uint64_t> count_items(const ItemFilter& filter) const;

    // Imports.
    [[nodiscard]] Expected<std::optional<ImportRecord>> find_import(ImportId id) const;
    // The most recent imports first. InvalidArgument for a bad limit.
    [[nodiscard]] Expected<std::vector<ImportRecord>> list_imports(std::uint32_t limit) const;

    // Recipe revisions (types.hpp). Reading a settings document verifies its SHA-256: a mismatch
    // is Corrupt. A revision origin this build does not know is Corrupt (VersionMismatch in a
    // catalog written by a newer schema).
    [[nodiscard]] Expected<std::optional<RecipeRevision>> find_revision(RevisionId id) const;
    // The item's current revision with its settings; std::nullopt when the item shows "as shot".
    // NotFound when the item does not exist.
    [[nodiscard]] Expected<std::optional<RecipeRevision>> current_revision(ItemId item) const;
    // Every revision of the item, oldest first, with parent links and the current pointer (no
    // settings documents). NotFound when the item does not exist.
    [[nodiscard]] Expected<RevisionHistory> revision_history(ItemId item) const;

    // Batch journal (schema v3, jobs.hpp). Reports read these; the coordinator reconciles with
    // them after a restart. A stored spelling this build does not know is Corrupt
    // (VersionMismatch in a catalog written by a newer schema).
    [[nodiscard]] Expected<std::optional<BatchRecord>> find_batch(BatchId id) const;
    // Batches, newest first, with ids below `before` (0: from the newest). InvalidArgument for a
    // bad limit.
    [[nodiscard]] Expected<std::vector<BatchRecord>> list_batches(std::uint32_t limit,
                                                                  BatchId before = {}) const;
    [[nodiscard]] Expected<std::optional<JobRecord>> find_job(JobId id) const;
    // InvalidArgument for a bad limit or an unknown stage in the filter.
    [[nodiscard]] Expected<std::vector<JobRecord>> list_jobs(const JobQuery& query) const;
    // Stage transitions in event order. InvalidArgument for a bad limit.
    [[nodiscard]] Expected<std::vector<JobEvent>> list_job_events(const EventQuery& query) const;
    // The job's journal entries in order (every attempt).
    [[nodiscard]] Expected<std::vector<PublicationRecord>> publication_journal(JobId job) const;
    [[nodiscard]] Expected<std::optional<ReceiptRecord>> find_receipt(JobId job) const;
    // Receipts of jobs with this identity, oldest first: outputs that may already exist for a new
    // job (verify the file against the receipt before relying on it).
    [[nodiscard]] Expected<std::vector<ReceiptRecord>> find_receipts_by_identity(
        const core::Digest256& identity) const;
    // Reconciliation: leased jobs whose lease expired at query.now, or (with live_owners) whose
    // owner is not live, in id order. Pass each lease to WriteTransaction::recover_job().
    [[nodiscard]] Expected<std::vector<JobRecord>> list_reclaimable_jobs(const ReclaimQuery& query) const;
    // Reconciliation: the latest entry of every attempt whose publication is in progress (neither
    // ReceiptCommitted nor Abandoned), in entry order, starting after entry id `after`. Scans the
    // journal's index (one row per entry): meant for start-up and occasional checks.
    [[nodiscard]] Expected<std::vector<PublicationInProgress>> list_publications_in_progress(
        std::optional<BatchId> batch, std::uint32_t limit, std::int64_t after = 0) const;

protected:
    ReadTransaction(detail::Connection& connection, bool newer_schema) noexcept
        : connection_(&connection), newer_schema_(newer_schema) {}
    ~ReadTransaction() = default;

    detail::Connection* connection_;
    bool newer_schema_;  // the catalog was written by a newer schema: unknown values are VersionMismatch

private:
    friend class Catalog;
    friend struct detail::CatalogAccess;
};

// Changes, valid inside Catalog::write() callbacks. Each call is atomic on its own (a failed call
// leaves no partial change behind), and the whole callback commits or rolls back as one
// transaction. Ids that do not exist are NotFound. Paths are validated with make_path_key() and
// must be valid UTF-16; text must be valid UTF-8 (InvalidArgument otherwise).
class WriteTransaction final : public ReadTransaction {
public:
    // Records a fresh observation of a file, keyed by make_path_key(observation.path):
    //  * no source with that key: inserts one (state Present) -> Inserted;
    //  * the catalogued identity (volume serial, file id, size, last write time) matches and the
    //    hashes do not contradict each other (either is absent or both are equal): keeps the
    //    stored hash when none is supplied, updates path spelling, mode, storage kind and
    //    last_seen, sets Present -> Unchanged, or Restored when it was Missing or Changed;
    //  * otherwise the file was replaced or modified: the identity and hash are replaced by the
    //    observation's (an absent hash clears the stored one), state Present -> Replaced. The
    //    caller must probe the file again and update its items.
    // first_seen is kept. Copied sources need origin_path (and may carry origin_identity);
    // Referenced sources must have neither (InvalidArgument). Mode, origin path and origin
    // identity are always replaced by the observation's.
    [[nodiscard]] Expected<SourceUpsertResult> upsert_source(const SourceObservation& observation);

    // Records the content hash of a source. `hashed` is the identity hash_file() reported for the
    // hashed bytes: when it differs from the catalogued identity the hash belongs to another
    // version of the file and SourceChanged is returned without a change.
    [[nodiscard]] Status set_source_hash(SourceId id, const core::Digest256& sha256,
                                         const core::FileIdentity& hashed);

    // Records the result of a check of the source's file. Present also sets last_seen to
    // `checked_at`; Missing and Changed keep the source, its items and flags (relink or reimport).
    [[nodiscard]] Status set_source_state(SourceId id, SourceState state, Timestamp checked_at);

    // Points a source at the file found at another path (see RelinkRequest), after the caller
    // verified it: by content (core::hash_file; the request's hash must then equal the catalogued
    // hash, InvalidArgument "catalog.relink.content_mismatch" otherwise) or by identity (the
    // request's identity equals the catalogued one exactly). A file verified neither way is refused
    // (InvalidArgument, "catalog.relink.unverified") unless allow_unverified is set. AlreadyExists
    // when another source has the new path. Sets path, identity, storage kind, state Present and
    // last_seen, and the hash when one is supplied (the stored hash is kept otherwise); items and
    // flags are kept. Returns the updated source.
    [[nodiscard]] Expected<SourceRecord> relink_source(SourceId id, const RelinkRequest& request);

    // Deletes a source and all of its items.
    [[nodiscard]] Status remove_source(SourceId id);

    // Inserts or updates the items of a source, matched by subimage index: probe columns are
    // replaced, flags are kept, new subimages get default flags. Items of subimages not listed are
    // left untouched (remove them explicitly). Returns the ids in the order of `items`.
    // InvalidArgument for repeated subimage indices, an orientation outside 1..8, a capture time
    // not in "YYYY-MM-DDTHH:MM:SS" form, invalid JSON or an unknown support status; NotFound for a
    // missing source.
    [[nodiscard]] Expected<std::vector<ItemId>> upsert_items(SourceId source,
                                                             std::span<const ItemMetadata> items);

    // Deletes items (all or nothing).
    [[nodiscard]] Status remove_items(std::span<const ItemId> items);

    [[nodiscard]] Status set_item_flags(ItemId id, ItemFlags flags);

    // Recipe revisions (types.hpp). Revisions are immutable and never deleted (they go with their
    // item); these calls only add revisions and move the item's current pointer.
    //
    // Stores `revision` as the item's next revision, derived from its current revision (the
    // parent; none when the item shows "as shot"), and makes it current, atomically. After an
    // undo this starts a new branch; the undone revisions are kept. Errors: NotFound (item),
    // NewRevision::validate()'s errors, InvalidArgument when settings_json is not valid JSON.
    // A caller that must not build on a current revision it has not seen (for example an
    // automatic run that started from an older state) reads current_revision() in the same
    // write() callback first.
    [[nodiscard]] Expected<RevisionInfo> append_revision(ItemId item, const NewRevision& revision);

    // Makes `revision` the item's current revision (an earlier recipe, a revision of another
    // branch), or "as shot" with std::nullopt. Errors: NotFound (item or revision),
    // InvalidArgument when the revision belongs to another item.
    [[nodiscard]] Status set_current_revision(ItemId item, std::optional<RevisionId> revision);

    // Undo: moves the current pointer to the parent of the current revision ("as shot" for a
    // revision derived from it) and returns the new current revision. InvalidArgument
    // ("catalog.undo_revision.nothing_to_undo") when the item already shows "as shot".
    [[nodiscard]] Expected<std::optional<RevisionId>> undo_revision(ItemId item);

    // Redo: moves the current pointer to the most recently created child of the current
    // revision (of "as shot": the newest revision without a parent) and returns it. After an
    // edit following an undo this follows the newest branch. InvalidArgument
    // ("catalog.redo_revision.nothing_to_redo") when there is no such revision.
    [[nodiscard]] Expected<RevisionId> redo_revision(ItemId item);

    // Import records. begin_import() creates a Running record; finish_import() completes it
    // exactly once (InvalidArgument when it is no longer Running or `finish.outcome` is Running).
    [[nodiscard]] Expected<ImportId> begin_import(const ImportStart& start);
    [[nodiscard]] Status finish_import(ImportId id, const ImportFinish& finish);
    // Marks every import still Running as Interrupted (finished_at = `now`). Call once at
    // application start, before new imports begin. Returns the number of records changed.
    [[nodiscard]] Expected<std::uint64_t> mark_interrupted_imports(Timestamp now);

    // ---- Batch journal (schema v3, jobs.hpp; blueprint section 7) ----------------------------
    //
    // Persist before dispatch: a batch and all of its jobs are created in one call, and every
    // later change is one atomic call that also appends its job_events. Because all writes go
    // through the single writer connection inside BEGIN IMMEDIATE, a claim is atomic: a job is
    // never leased to two owners. Stable error locations (Error::where) callers can test:
    //   "catalog.job.lease_lost"           the lease is not (or no longer) the job's: expired,
    //                                      recovered, superseded, or the job left the leased
    //                                      stages (InvalidArgument)
    //   "catalog.job.stage_conflict"       the job is not in the stage the call requires
    //                                      (InvalidArgument; optimistic check)
    //   "catalog.job.no_receipt"           Complete without a receipt (InvalidArgument)
    //   "catalog.job.journal_order"        a publication step out of order (InvalidArgument)
    //   "catalog.job.batch_cancelled"      advancing a job of a cancelled batch before
    //                                      publication (Cancelled)
    //   "catalog.batch.invalid_transition" a batch command its state does not allow
    //                                      (InvalidArgument)
    //   "catalog.receipt.conflict"         a different receipt for a job that has one
    //                                      (AlreadyExists)
    // Ids that do not exist are NotFound. A batch whose jobs all reach terminal stages becomes
    // Completed (from Running or Paused) in the call that finishes its last job; a cancelled
    // batch gets its finished_at then.

    // Creates a batch (Draft, or Running with NewBatch::start) and its jobs (Queued, ordinals in
    // span order), computes every job identity and appends one creation event per job. Each item
    // must exist and its source must be hashed (InvalidArgument "catalog.create_batch.unhashed":
    // the identity needs the content hash); a revision must belong to its item and match the
    // job's settings_sha256. InvalidArgument for no jobs or invalid JSON; LimitExceeded above
    // kMaxBatchJobs or the size limits.
    [[nodiscard]] Expected<CreatedBatch> create_batch(const NewBatch& batch, std::span<const NewJob> jobs);

    // Batch commands; each returns the batch's state afterwards. Repeating a command in the state
    // it produces is a no-op.
    //   start:  Draft -> Running
    //   pause:  Running -> Paused (leased jobs continue; nothing new is dispatched)
    //   resume: Paused -> Running
    //   cancel: Draft / Running / Paused -> Cancelled: queued jobs become Cancelled; leased jobs
    //           are asked to stop (LeaseRenewal::cancel_requested). Completed cannot be cancelled.
    [[nodiscard]] Expected<BatchState> start_batch(BatchId id, Timestamp now);
    [[nodiscard]] Expected<BatchState> pause_batch(BatchId id, Timestamp now);
    [[nodiscard]] Expected<BatchState> resume_batch(BatchId id, Timestamp now);
    [[nodiscard]] Expected<BatchState> cancel_batch(BatchId id, Timestamp now);
    // Requeues the batch's Failed jobs (claimable from `now`; failure counts are kept) and makes
    // a Completed batch Running again (a Running or Paused batch keeps its state).
    // "catalog.batch.invalid_transition" for a Draft or Cancelled batch. Returns the number of
    // jobs requeued.
    [[nodiscard]] Expected<std::uint64_t> retry_failed_jobs(BatchId id, Timestamp now);

    // Leases the next Queued job of a Running batch (oldest batch first, then ordinal) whose
    // not_before has passed: stage Rendering, attempt + 1, lease until now + lease_duration.
    // std::nullopt when nothing is claimable. InvalidArgument for a bad owner or duration.
    [[nodiscard]] Expected<std::optional<ClaimedJob>> claim_next_job(const ClaimRequest& request);

    // Extends a valid lease to now + lease_duration and reports whether the batch was cancelled.
    // An expired lease cannot be renewed ("catalog.job.lease_lost").
    [[nodiscard]] Expected<LeaseRenewal> heartbeat_job(const JobLease& lease, Timestamp now,
                                                       std::chrono::milliseconds lease_duration);

    // Moves a leased job forward one stage, from `from` (which must be its stage: optimistic
    // check) to the next: Rendering -> Verifying -> Publishing -> Complete. Complete needs the
    // job's receipt ("catalog.job.no_receipt") and ends the lease. In a cancelled batch only
    // Publishing -> Complete is allowed. Returns the new stage.
    [[nodiscard]] Expected<JobStage> advance_job(const JobLease& lease, JobStage from, Timestamp now);

    // Appends a publication step for the lease's attempt. Steps go strictly in order Reserved,
    // TempWritten, Verified, Published, each once per attempt; paths and output hash are
    // required as PublicationEntry describes, and the job's temp_path / final_path follow the
    // latest entry. Published requires stage Publishing.
    [[nodiscard]] Status record_publication_step(const JobLease& lease, const PublicationEntry& entry,
                                                 Timestamp now);

    // Records the receipt of a published output (stage Publishing; the attempt's last journal
    // step must be Published with the same final path, size and hash) and appends
    // ReceiptCommitted. Idempotent: the same receipt again returns the stored one unchanged; a
    // different one is AlreadyExists ("catalog.receipt.conflict").
    [[nodiscard]] Expected<ReceiptRecord> record_receipt(const JobLease& lease, const NewReceipt& receipt,
                                                         Timestamp now);

    // Records a failure of a leased job and ends the lease: Queued (claimable from retry_at) when
    // retry_at is set, otherwise Failed. Increments the failure count of its class. In a
    // cancelled batch a requeue becomes Cancelled. Returns the new stage.
    [[nodiscard]] Expected<JobStage> record_failure(const JobLease& lease, const JobFailure& failure,
                                                    Timestamp now);

    // Gives a leased job back without a failure (pause at a safe boundary, memory admission):
    // Queued, claimable from `now` (Cancelled in a cancelled batch). Returns the new stage.
    [[nodiscard]] Expected<JobStage> release_job(const JobLease& lease, Timestamp now);

    // The worker stopped a leased job on request: Cancelled. Refused ("catalog.job.stage_conflict")
    // once the attempt has Published its output: finish it with record_receipt and Complete.
    [[nodiscard]] Status cancel_job(const JobLease& lease, Timestamp now);

    // Skips a Queued job (for example: its output already exists, or its source is missing),
    // recording `reason`. "catalog.job.stage_conflict" for any other stage.
    [[nodiscard]] Status skip_job(JobId job, const Error& reason, Timestamp now);

    // Reconciliation of an abandoned lease (from list_reclaimable_jobs): `stale` must still be the
    // job's lease (owner and attempt), expired at recovery.now unless recovery.owner_gone
    // ("catalog.job.lease_lost" otherwise). Requeue and Fail record recovery.error as a failure of
    // recovery.failure_class; Complete requires a receipt (stored, or recovery.receipt, which must
    // match the attempt's Published journal entry and is then written with ReceiptCommitted): an
    // abandoned lease is never completed without one. Returns the new stage.
    [[nodiscard]] Expected<JobStage> recover_job(const JobLease& stale, const JobRecovery& recovery);

    // Marks an interrupted publication as reconciled (after the caller removed its temporary
    // file and checked or released its final name) by appending Abandoned to the attempt, and
    // clears the job's temp_path. "catalog.job.stage_conflict" while that attempt still holds the
    // lease (recover the job first); "catalog.job.journal_order" when the attempt has no open
    // publication.
    [[nodiscard]] Status abandon_publication(JobId job, std::uint32_t attempt, Timestamp now);

private:
    friend class Catalog;
    friend struct detail::CatalogAccess;
    explicit WriteTransaction(detail::Connection& connection) noexcept : ReadTransaction(connection, false) {}
    ~WriteTransaction() = default;
};

class Catalog {
public:
    // Opens or creates a catalog (see the file comment for the full sequence). Errors
    // ("catalog.open"): InvalidArgument (bad options), Unsupported (network or unknown location, a
    // database that is not a Lucent catalog), NotFound (missing file and !create_if_missing, or
    // ReadOnly), VersionMismatch (newer schema refused), Corrupt (integrity check failed),
    // AlreadyExists (opened ReadWrite elsewhere), migration and backup failures, I/O errors.
    [[nodiscard]] static Expected<std::unique_ptr<Catalog>> open(const OpenOptions& options);

    // Closes every connection. The last connection checkpoints the WAL into the database file.
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;  // absolute
    [[nodiscard]] StorageKind storage_kind() const noexcept;
    // True when writes are refused: OpenMode::ReadOnly, or a newer schema (schema().read_only_reason).
    [[nodiscard]] bool is_read_only() const noexcept;
    [[nodiscard]] const SchemaStatus& schema() const noexcept;
    [[nodiscard]] const std::filesystem::path& backup_directory() const noexcept;

    // Runs `work` in one read transaction on a pooled read connection (waiting while all
    // max_readers connections are in use) and returns its status. Do not call read() or write()
    // from inside `work`: a nested read() can deadlock when the pool is exhausted, and inside
    // write() use the WriteTransaction's own queries. Exceptions thrown by `work` are caught:
    // std::bad_alloc becomes OutOfMemory, anything else Internal.
    [[nodiscard]] Status read(const std::function<Status(ReadTransaction&)>& work) const;

    // Runs `work` in one write transaction (BEGIN IMMEDIATE) on the writer connection, holding the
    // writer lock. Commits when `work` returns success; rolls back everything when it returns an
    // error or throws, and returns that error. A read-only catalog fails with PermissionDenied
    // (OpenMode::ReadOnly) or its read_only_reason (VersionMismatch). A nested write() on the
    // same thread fails with Internal instead of deadlocking.
    [[nodiscard]] Status write(const std::function<Status(WriteTransaction&)>& work);

    // Convenience forms returning the callback's value:
    //     Expected<std::optional<SourceRecord>> s = catalog.query([&](ReadTransaction& tx) {
    //         return tx.find_source(id); });
    // `work` returns Expected<T> (or Status); the transaction commits only when it has a value.
    template <class F>
    [[nodiscard]] auto query(F&& work) const -> std::invoke_result_t<F&, ReadTransaction&> {
        return run_with_result<ReadTransaction>(work, [this](const auto& fn) { return read(fn); });
    }

    template <class F>
    [[nodiscard]] auto transact(F&& work) -> std::invoke_result_t<F&, WriteTransaction&> {
        return run_with_result<WriteTransaction>(work, [this](const auto& fn) { return write(fn); });
    }

    // Writes a verified copy of the catalog to `destination` (an absolute path whose folder
    // exists; any drive, including network and removable ones). Uses SQLite's online backup API
    // on a read snapshot, so it runs concurrently with reads and writes and captures one committed
    // state. The copy is written to a temporary sibling, checked (integrity_check, application id,
    // schema version), switched to a self-contained rollback-journal file and then renamed into
    // place without replacing anything: AlreadyExists when `destination` exists.
    [[nodiscard]] Expected<BackupInfo> backup_to(const std::filesystem::path& destination) const;

    // backup_to() into backup_directory() with a generated name
    // "<catalog file name>.<UTC yyyymmddThhmmssfffZ>.v<schema>.manual.bak", then deletes the oldest
    // manual backups beyond backups_to_keep.
    [[nodiscard]] Expected<BackupInfo> create_backup() const;

    // Backups of this catalog in backup_directory(), newest first (files whose names follow the
    // pattern above; anything else in the folder is ignored).
    [[nodiscard]] Expected<std::vector<BackupInfo>> list_backups() const;

    // Replaces the catalog at `catalog_path` with the backup at `backup_path`, or creates it (and
    // its folder) when there is none. The catalog must not be open: AlreadyExists when this or
    // another process has it open ReadWrite; a sharing error (IoError / PermissionDenied) when
    // another program holds its files open, detected before anything is replaced. Steps:
    //  1. verify the backup (integrity_check, foreign_key_check, application id) and refuse a
    //     schema newer than kSchemaVersion (VersionMismatch); an older one is migrated by the
    //     next open();
    //  2. if a catalog exists, take a verified PreRestore online backup of it; if it cannot be read
    //     (corrupt), move its database and WAL files aside instead (RestoreReport::moved_aside);
    //  3. copy the backup to a verified temporary sibling, delete the old WAL and shared-memory
    //     files and rename the copy over the catalog.
    // On failure before step 3 completes, the existing catalog is left as it was. Network
    // locations are refused as in open(). Errors use "catalog.restore".
    [[nodiscard]] static Expected<RestoreReport> restore(const std::filesystem::path& catalog_path,
                                                         const std::filesystem::path& backup_path,
                                                         const RestoreOptions& options = {});

    // Copies the WAL into the database file and truncates it (PRAGMA wal_checkpoint(TRUNCATE)),
    // waiting for the writer lock. Useful before an application update or after a large import.
    // Readers that hold old snapshots can make the checkpoint partial; that is not an error.
    [[nodiscard]] Status checkpoint();

private:
    struct Impl;
    friend struct detail::CatalogAccess;
    explicit Catalog(std::unique_ptr<Impl> impl) noexcept;

    template <class Tx, class F, class Runner>
    static auto run_with_result(F& work, const Runner& runner) -> std::invoke_result_t<F&, Tx&> {
        using Result = std::invoke_result_t<F&, Tx&>;
        std::optional<Result> result;
        const Status status = runner(std::function<Status(Tx&)>([&](Tx& tx) -> Status {
            result.emplace(work(tx));
            if (!result->has_value()) {
                return Unexpected(result->error());
            }
            return ok();
        }));
        if (!status) {
            return Unexpected(status.error());
        }
        return std::move(*result);
    }

    std::unique_ptr<Impl> impl_;
};

}  // namespace lucent::catalog
