# ADR 0012: Durable batch export: worker processes, memory-budgeted scheduling, journaled publication and recovery

- **Status:** Accepted (amended 2026-10-09: "Amendment: aggregate worker memory limit (W12)" below, with its closing review;
  amended 2026-10-10 by the closing review of W13: "Amendment: lens corrections in job results" below, protocol version 3)
- **Date:** 2026-10-04 (revised the same day by the closing review, phase 4: recovery that waits for
  an unreachable destination, interruptions bounded on their own, lease renewal during verification,
  a replaced output folder, lost receipts, report emission, single-export priority and the status
  the interface needs; decision 14 added)
- **Workflow:** W7 (Durable batch), phases 2 and 4 of 4

## Context

Blueprint section 7 ("Durable, memory-bounded batch processing") asks for a job lifecycle that is
persisted before dispatch and reconciled after a restart ("Do not mark an abandoned worker lease
complete; safely retry from the latest valid stage"), transient retries bounded separately from
deterministic failures, actionable states for "disk-full, removed drive, permission loss, and missing
source", pause and cancel at safe boundaries, an aggregate memory budget ("approximately 8 GB total
Lucent budget ... Start with one full-resolution worker. Admit a second only after measurements show
both fit comfortably"), reserved interactive capacity, and export integrity: a temporary sibling on
the destination volume, verification before publication, no-clobber publication with destination
identity re-checked, receipts with output hash and settings, "a publication journal" reconciled
between each step, and "On restart, verify an existing output against its receipt before counting it
complete". Section 2 ("Automatic batch development") adds the destination choices (output folder,
JPEG / TIFF preset, naming pattern, metadata preference, collision policy, example destination
paths), the progress, pause / cancel, "Open output folder" and the report of exported, skipped, failed
and review-recommended items. Section 4 puts decoding and encoding into worker processes "for crash
containment and release of native allocations"; section 8 versions the worker protocol and the
job records.

Phase 1 of W7 delivered the parts this phase composes (all committed on this branch):

- catalog schema v3 (`catalog/jobs.hpp`): batches, jobs with a content-derived identity, leases and
  heartbeats, append-only events, the ordered publication journal (Reserved → TempWritten → Verified
  → Published → ReceiptCommitted, or Abandoned) and immutable receipts, with triggers that forbid
  Complete without a receipt;
- `lucent_workers` and `lucent-worker.exe`: the versioned protocol "lucent.worker" v1 (length-prefixed
  JSON over two inherited anonymous pipes, frozen handshake, identical builds required), a
  `WorkerHost` per process in a Job Object (kill on close, optional commit limit, below-normal
  priority), heartbeats, cooperative cancel then termination, fault classification, and a worker
  that renders one job, writes, re-reads, fully decodes, compares and hashes its output into the
  host's temporary path; pixels never cross the pipe;
- `exporter` EXIF metadata with an explicit GPS policy and a capture-metadata reader.

The W2 deferral (ADR 0006): "holding the reserved temporary file's handle for the whole protocol
instead of reopening it by name" and sweeping `~lucent-*.tmp` files belong to W7.

## Decision

1. **One coordinator in Lucent.exe, isolated workers** (`app::BatchService`, `batch_service.hpp`).
   The coordinator is pure C++ in `lucent_application` (no Qt): one coordinator thread, and one slot
   thread per worker slot (`max_workers`, 1 to 4, default 2), each driving one `WorkerHost`. Threads
   are therefore bounded at 1 + `max_workers`; no thread is created per job. The coordinator claims
   jobs (interactive single exports first, `BatchRequest::single_export`, so an export started from
   Edit waits for at most the attempts already running and not for the rest of an earlier batch;
   then the oldest running batch, then batch order), the slot runs the attempt (worker job,
   verification, publication, receipt) and gives the slot back. The coordinator looks only at the
   unfinished batches, an in-memory set filled at recovery by a scan of every batch and kept by the
   service's own commands (it is the catalog's only batch writer; only dispatch drops a batch it read
   as finished, and never one reopened meanwhile), so any number of finished batches — every single
   export is one — costs nothing per dispatch. A worker process is reused for its
   slot's jobs and restarted after any fault. The UI's `Backend` opens the service over the writable
   persistent library (`BackendOptions::batch`) and passes `InteractiveActivity::busy` as
   `interactive_busy`; the batch user interface is phase 3's.

2. **Protocol choice and isolation limits** (phase 1, recorded here). Anonymous pipes created by the
   host and inherited by the worker alone were chosen over named pipes (no name to squat, no DACL
   to get wrong) and over shared memory (pixels never cross; only paths, identities, settings,
   hashes and measurements do). Isolation is crash containment and memory release, not a security
   sandbox: the worker runs as the user with the user's rights. Each worker process gets a Job Object
   commit limit equal to the worker budget (decision 4) and the engine's refusal threshold
   (`JobRequest::memory_budget_bytes`) is the same value, so a photo too large for the budget fails
   with LimitExceeded up front (an insufficient-memory result) and is never exported smaller. (Since
   W12 every worker's Job Object also nests in one group Job Object whose limit is the worker budget
   for all workers together: decision 17.) The engine inside a worker still sizes its own thread
   pool from the core count: with two workers the
   pools can oversubscribe the CPU; worker processes run below normal priority so the interface and
   interactive previews keep the CPU. A per-job thread limit needs a protocol field (requested from
   the workers owner).

3. **Batches and jobs.** `submit()` validates the destination (`BatchDestination`, decision 9) and
   the output settings (`exporter::ExportSettings`: JPEG / sRGB / quality 95, or 16-bit TIFF with
   sRGB or Display P3; `MetadataPolicy` with capture EXIF, artist / copyright, software, and GPS off
   unless chosen; optional artist / copyright text), creates the output folder when missing, proves
   it writable with a probe file, requires room for the largest single output, and selects the
   items (a selection, or a folder that is first imported and then every library item inside it).
   Items that cannot be exported are excluded with their reason (unknown, Unsupported unless
   `allow_unsupported`, Missing, Changed, unreadable recipe, a source that cannot be hashed, an
   output path above 32,000 characters) and listed in the report. Each job freezes the item's recipe
   revision (or the as-shot defaults; settings without a revision that are not as shot are refused,
   since the journal could not reproduce them), the source's content hash (sources without one are
   hashed now, with a stable read), the export settings and the renderer build: the job identity
   (`catalog::compute_job_identity`; renderer version "lucent <build>; develop <engine versions>;
   worker protocol 1"). With `ExistingOutputs::Skip` (default), a job whose identity already has a
   receipt for a file in the same destination folder that still hashes to the receipt is skipped
   "already exported" in the same transaction that creates the batch; the skipped job records that
   file (its `final_path`), so the report names the output submission verified rather than any
   receipt of the identity. The batch and all of its jobs are one catalog transaction (persist
   before dispatch). `plan()` does steps without writing or hashing and returns the example
   destination paths (outputs that will be written first, then already exported ones, flagged),
   the counts and stable `PlanNotice` codes (network or removable destination, replacing, GPS, may
   run out of space, too full for one output) the UI words itself; it counts an earlier output as
   already exported when its file has the receipt's size, and submission confirms that byte for
   byte with a cancellable hash, so a dialog that replans on every change never re-reads gigabytes
   of earlier outputs. It never imports, so for a folder batch it covers the folder's photos
   already in the library (none: "app.batch.empty"); the interface imports the folder first, or
   shows the examples once the folder's photos are listed.

4. **Scheduler and memory budget.** Aggregate budget = `BatchMemoryOptions::total_bytes` or half the
   physical memory clamped to 2..8 GiB (8 GiB on the 16 GB reference laptop; 5.95 GiB on the
   11.9 GB development machine); the interactive reserve (1.5 GiB: Lucent.exe's previews, edit
   sessions and caches) is subtracted to give the worker budget (4.45 GiB here). A job's estimate is
   `max(160 MiB, measured floor) + pixels × bytes-per-pixel`, starting at 24 bytes per pixel (the
   engine's estimate for an export render) and replaced, once measured, by the largest measured
   `(peak commit − baseline) / pixels` of photos above 1 MP times 1.15. Admission: nothing starts
   while `interactive_busy()` holds (running jobs continue); the first worker always runs; another is
   admitted only after a job has been measured, when the estimates (or larger observed commits) of
   the running jobs plus the next fit the worker budget, and when the system's available physical
   memory exceeds the next estimate by 512 MiB. Peak commit is taken from the worker's heartbeats
   and, for a process started for the job, from Job Object accounting.

5. **Leases and failures.** A claim's lease (60 s) is renewed every 10 s from the worker's
   heartbeats, between protocol steps and before every lease-checked catalog write, and while the
   coordinator verifies the output (between the 1 MiB reads of its re-hash, and before the metadata
   read-back), so a large TIFF on a slow share keeps its lease however long verification takes; a
   renewal that reports the batch cancelled cancels the attempt. Lease owners are unique per attempt
   ("<session>/slot<n>/<counter>"). An attempt that loses its lease before publication (it expired:
   the computer slept, or a step outlasted it) is an *interruption*: its temporary file is removed
   and, while nobody else took the lease, the job is requeued at once (`recover_job` with
   `interruption`), recorded as "app.batch.interrupted.lease" (not as "Lucent stopped"). Interruptions
   — this one, and a coordinator that stopped (recovery) — are counted in `jobs.interruptions`, not
   in the transient failures, so they never use up the retry budget of genuine worker failures; they
   are bounded on their own (`BatchRetryPolicy::max_interruptions`, 5): the next one fails the job
   ("app.batch.interrupted.limit", "Lucent was interrupted repeatedly while exporting this photo").
   After publication a lost lease, journal or receipt write leaves the job to reconciliation, at once
   (the slot asks for a reclaim when it is free). Failures are diagnosed after every failed attempt,
   in this order:
   - *destination problems* (the output folder missing, its drive absent, another folder at its path
     — the folder's volume serial and file id are recorded at submission —, DiskFull or free space
     below the next output's uncompressed size plus 64 MiB, PermissionDenied confirmed by a write
     probe): the job is given back without counting a failure and the batch pauses with a
     `BatchAttention` (DestinationFull with the free and needed bytes, DestinationUnavailable,
     DestinationPermission, DestinationReplaced) and an Attention event, so the remaining jobs wait
     instead of all failing; `resume()` clears it and re-checks. The coordinator also checks the
     destination before each claim, and every attempt checks that the output folder is there with
     its recorded identity before it creates anything: the output folder itself is never created
     after submission (only mirrored subfolders below it), so a folder the user deleted is reported
     instead of being made again as another folder object. Another folder object at its path (the
     user created it again, or another drive got its letter) is DestinationReplaced: resume() raises
     it again, and `use_replaced_folder()` (the interface's "Use this folder") records the new
     folder's identity in the batch (`set_batch_destination`) and resumes;
   - *source problems*: the original absent → Failed (NotFound, "app.batch.source.missing": reconnect
     or relink, then Retry failed); its identity or content changed → Failed (SourceChanged:
     re-import and export in a new batch); a PermissionDenied while the destination still accepts
     files and the original cannot be opened for reading → Failed (PermissionDenied,
     "app.batch.source.access": fix the file's permissions, then Retry failed); all fail fast (one
     attempt);
   - otherwise by class: a photo found unsupported while decoding is Skipped with its reason (as the
     photos left out at submission: not a failure of the batch, and retrying cannot help);
     deterministic failures (corrupt input, a refused recipe, the worker's memory limit) fail at
     once; transient ones (a worker crash, hang or timeout, I/O, memory pressure, a verification
     failure, a name taken during the rename) are retried after `initial_backoff × 2^n` (2 s, capped
     at 60 s) while the job's transient failures stay below `max_transient_failures` (3), then fail.
     `retry_failed()` requeues failed jobs, except those whose receipt committed (their published
     output was lost afterwards): receipts are one per job and immutable, so such a photo is exported
     again in a new export, as its report entry says.
   Other jobs and other batches continue in every case.

6. **Publication protocol** (journal steps in brackets; one attempt on its slot thread):
   - [Reserved] the final folder (mirrored subfolders created as needed) and name: the pattern's
     name, or under UniqueName the first with "_2", "_3", ... that is free in a listing of the folder
     (case-insensitive), by exact path on disk, and among the names other running attempts reserved.
     The worker's temporary file is `~lucent-<tag>-<job>-a<attempt>.tmp` in the final folder: the same
     volume, unique per attempt (the batch's random 8-hex-digit tag keeps two catalogs apart), and a
     stale file of that name is removed before use. Reserved is recorded before the worker starts,
     so every temporary file is in the journal.
   - [TempWritten] after the worker answered: it rendered, encoded, wrote through the exporter,
     re-read, fully decoded and compared the bytes (JPEG) or samples (TIFF), checked the metadata and
     re-hashed the file at its temporary path.
   - [Verified] the coordinator opens the temporary file **once** (`HeldOutput`, read + write for
     the flush + DELETE, shared for reading only) and keeps that handle until the file is published:
     nobody can modify, rename, replace or delete it meanwhile. Through the handle it re-hashes the
     file (size and SHA-256 must equal the worker's), parses the container header itself (JPEG frame
     header and ICC_PROFILE chunks; classic TIFF IFD 0) and requires the chosen format, dimensions
     equal to the render's, three channels of 8 (JPEG) or 16 (TIFF) bits, and an embedded ICC profile
     byte-identical to the chosen profile's; the worker's verification flags, recipe hash, format,
     profile and bit depth must match the request; the EXIF read back must hold GPS, capture, rights
     and software only as the metadata policy allows. The decode is the worker's; the hash binds it
     to these bytes. The final name is checked again: a file that appeared meanwhile is never
     replaced (the next free name is reserved), and under Overwrite the file to replace is identified
     now. This closes ADR 0006's deferral: what is verified is the file object that is published,
     never "whatever is at the temporary name".
   - [Published] the held file is renamed by handle (`SetFileInformationByHandle`, FileRenameInfo
     without ReplaceIfExists) to the final name and flushed. A file created at that name after the
     check makes the rename fail with AlreadyExists: nothing is ever clobbered, and the attempt is
     retried (transient). Under `CollisionPolicy::Overwrite` (explicit; never an original — a file any
     library item renders, checked by path and file identity —, never another output of the same
     batch) the existing file is opened with no sharing, its identity revalidated against the one
     seen at verification, renamed aside to `~lucent-<tag>-<job>-a<attempt>-replaced.tmp`, the output
     renamed into place (the aside file is put back if that fails), and the aside file removed after
     the Published step.
   - [ReceiptCommitted] the receipt (final path, size, SHA-256 and the "lucent.batch-receipt" v1
     details: recipe and export-settings hashes, settings, format, profile, bit depth, dimensions,
     ICC hash, chroma subsampling, encoder, metadata notes, render summary, timings, attempt, name
     index, replacement, review notes) is committed, then the job completes.
   Pause and cancel act only at the safe boundaries before the rename; after it the attempt always
   completes with its receipt.

7. **Recovery** runs when the service opens, on the coordinator thread before anything is
   dispatched (so a slow or disconnected destination never delays Lucent's start-up;
   `wait_for_recovery()` lets callers wait). Every job still leased by a previous session (no worker
   survives its coordinator: Job Objects end them) is reconciled from its journal and receipt:

   Nothing is decided about outputs recovery cannot see. First the batch's output folder is
   checked as the coordinator does before dispatch (it opens, and it is the file object recorded at
   submission); a folder out of reach (a USB drive not plugged in yet, a mapped share not
   reconnected) or replaced leaves the job leased and its publication open. Then each file is
   compared as Match, Mismatch (absent with its folder reachable, another size, other bytes) or
   Unknown (it cannot be read now: a sharing violation from a sync client or a virus scanner, an
   I/O error); Unknown leaves the job for later too. Deferred jobs and publications (counted in
   `RecoveryReport::deferred`, logged once) are tried again every `reclaim_interval` (30 s) and at
   once on `resume()`, and the leftover sweep keeps their temporary and set-aside files. Otherwise:

   | Journal of the interrupted attempt | What recovery does |
   |---|---|
   | receipt committed | Match: Complete; Mismatch: Failed "the published output is missing or was changed" (it cannot be retried in place, decision 5) |
   | Published | Match against the journal's size and hash: Complete with a recovered receipt (review note: render details not recorded); Mismatch: requeue |
   | Verified | the rename may have happened: if the temporary file is gone and the final name holds exactly the verified bytes and no receipt names that path, that file is the attempt's unrecorded output and is removed; a file an overwrite moved aside is put back; requeue |
   | Reserved, TempWritten | remove the temporary file and its `.parts` scratch folder; requeue |

   Requeueing records the interruption ("app.batch.interrupted": Lucent stopped; or the lease
   expired in this session) as an interruption (decision 5, bounded by `max_interruptions`), and the
   attempt's publication is closed (Abandoned). Then publications left open by attempts that no
   longer hold a lease are closed the same way, and the output folders of unfinished batches (and of
   those just reconciled) are swept: this catalog's `~lucent-<tag>-*` files and scratch folders, and
   the exporter's own `~lucent-<16 hex>.tmp` temporaries older than an hour (left by a killed
   in-process export, ADR 0006) that no program holds open. Finally batches that finished without
   their report (the coordinator stopped between the last Complete and the report files:
   `batches.reported_at` is unset, decision 14) get it when their folder is reachable. While
   running, leases that expire without a live attempt are reconciled the same way every
   `reclaim_interval`. Consequently an output is never published twice, every published output has
   exactly one receipt, no partial or unrecorded output stays in the folder, and a verified output is
   never failed or rendered again because its folder was out of reach at start-up. Closing the
   service cleanly gives running jobs back (Queued, no failure), so a batch continues at the next
   start.

8. **Pause, cancel, resume.** Pause stops dispatch; each running attempt finishes (its output is
   published — the safe boundary is the end of the attempt, since a verified output costs only a
   rename). Cancel cancels queued jobs and asks running workers to stop (cooperative cancel, then
   termination after `cancel_grace`); an attempt that already renamed its output completes; published
   outputs stay valid. Resume continues and clears a BatchAttention.

9. **Names and destinations** (`batch_destination.hpp`). Pattern tokens `{original-stem}`,
   `{revision}` (the recipe revision number, 0 for as shot), `{image}` and `{sequence}` (four
   digits); default `{original-stem}_Lucent_{revision}`; a pattern must contain `{original-stem}` or
   `{sequence}`, may not contain a separator or a character Windows forbids, and outputs of
   multi-image files without `{image}` get "_img{k}". Expansion removes trailing dots and spaces,
   prefixes reserved device names (CON ... LPT9, their superscript forms, CONIN$, CONOUT$) with "_",
   shortens the name part (never splitting a surrogate pair) to fit 255 UTF-16 units with "_n" and
   the extension, and mirrored subfolder components are sanitised the same way. Collisions are
   case-insensitive (the file system's casing rules), also inside a batch (planned in batch order, so
   the examples shown match the outcome while the folder does not change). Long paths are handled
   with "\\?\" forms throughout; outputs above 32,000 characters are excluded.

10. **Documents in the journal.** `destination_json` ("lucent.batch-destination" v1: folder and its
    file object, pattern, mirroring and its root, collision policy, temporary tag, report files,
    storage kind), `export_settings_json` ("lucent.batch-export" v1, written with sorted members and
    no whitespace because its SHA-256 is part of every identity: format, profile, intent,
    black-point compensation, quality, compression, metadata policy, rights text, allow_unsupported)
    and `policy_json` ("lucent.batch-policy" v1: existing-output policy, retry policy, exclusions).
    Readers accept exactly version 1 (a newer one is VersionMismatch). The frozen settings document
    of a job is re-read from its revision (immutable) and must hash to the job's settings SHA-256.

11. **Completion report.** When a batch finishes (Completed, or Cancelled with nothing running), its
    report is written into the output folder before the Finished event (unless
    `write_report_files` is false): `Lucent-batch-<id>-report.json` ("lucent.batch-report" v1: the
    batch, its settings and destination documents, the summary counts, notes, and one entry per job
    and per exclusion with outcome, output path, size, SHA-256, user-facing reason and action, review
    notes, attempts and the developer error) and a self-contained `Lucent-batch-<id>-report.html`
    (light and dark). Outcomes: exported, skipped (already exported, excluded at submission, or found
    unsupported while decoding), failed, cancelled, pending. Review is recommended for limited
    support, renderer warnings, more than 1 % of pixels beyond the output's white, changed or dropped
    metadata, a renamed output, a replaced file, an original's name being avoided, and a size that
    differs from the catalogued one (W8 adds the optimiser's confidence). `report()` returns the same
    data; `output_folder()` is what "Open output folder" opens; `status()` has the running jobs, the
    attention (with the free and needed bytes of a full destination), an ETA once three jobs of the
    session have completed and only while the batch runs without an attention, whether it is a
    single export, the photos left out at submission, the exported outputs recommended for review
    and the failed jobs `retry_failed()` requeues (read with one catalog query, cached while the
    counts do not move), and for a batch of one its output or why there is none (user-facing, as
    the report words it), so the interface never reads reports to draw its list. Batches are listed
    newest first in pages (`list(limit, before)`), and `list_unfinished()` returns every batch with
    work left whatever its age. Reports are written once per completion: `batches.reported_at`
    records it (retry_failed() clears it), and a report that could not be written (its folder out of
    reach) is written at the next start.

12. **Single exports are batches of one.** `app::ExportService` given a `BatchService` exports
    through a batch of one photo with the W3 names (`{original-stem}_Lucent`, unique), the request's
    output settings, `ExistingOutputs::ExportAgain` (asking again makes a new file, as in W3), no
    report files and `single_export` (dispatched first, shown as one photo); its executor task waits
    for the batch and builds the `ExportRecord` from the receipt, so the shell and its tests see the
    same contract. A destination attention (full disk, missing folder) cancels the batch with that
    reason recorded on the cancelled job (`cancel(batch, reason)`, `cancel_batch_with_reason`), so the
    Exports view and the report say why, and fails the export with that error, as in W3. The in-process
    path remains only where the journal cannot be used: a session library (no catalog: tests and the
    "Library not saved" fallback) and explicit settings that no revision records. Reason for
    choosing it: one integrity path (verification, no-clobber, receipts, recovery) and crash
    containment for every export; the cost is a worker start per export when no worker is running
    (about 0.1 s) and a source hash when the photo was not hashed yet.

13. **Packaging.** `lucent-worker.exe` is installed next to `Lucent.exe`
    (`LUCENT_PACKAGE_HELPER_TARGETS`; building Lucent builds it), its runtime DLLs are resolved with
    the application's, and the package audit requires exactly the executables Lucent.exe and
    lucent-worker.exe, both GUI-subsystem programs identical to the build; signing covers both, and
    static libraries of both are checked against the licence policy (`lucent-package.json` schema 3).

14. **Schema v4** (catalog, additive; minimum reader version stays 2): `jobs.interruptions`
    (decision 5) and `batches.reported_at` (decision 11; batches finished before the migration count
    as reported). New catalog calls: `list_unfinished_batches`, `list_unreported_batches`,
    `batch_job_facts` (failed jobs without a receipt, receipts flagged at a JSON path),
    `set_batch_destination`, `mark_batch_reported`, `cancel_batch_with_reason`;
    `retry_failed_jobs` leaves jobs with a receipt alone; `record_failure` can skip
    (`JobFailure::skip`); `skip_job` can name the existing output; `recover_job` can record an
    interruption. The policy document gains the optional members `retry.max_interruptions` and
    `single_export` (version 1 readers take their defaults when absent).

## Consequences

Measured on the development machine (Ryzen 5 5500U, 11.9 GB, another workflow building at the same
time), 2026-10-04, `lucent_batch_fixture_tests` and `lucent_batch_tests`:

| What | Result |
|---|---|
| Budget on this machine | total 5.95 GiB, worker budget 4.45 GiB, interactive reserve 1.5 GiB |
| 5 smoke fixtures to JPEG sRGB q95 (the DJI DNG is excluded: mandatory WarpRectilinear) | 10.5 to 13.7 s for the batch (10.6 s run alone), two workers admitted after the first measurement (the four jobs' own times add up to about 15 s) |
| Canon EOS R5 45 MP to JPEG: render / encode / write / coordinator verification | 1.0 / 2.2 / 0.8 / 0.02 s; total 4.1 to 4.4 s |
| Worker peak commit, R5 JPEG / R5 16-bit Display P3 TIFF | 731 MiB / 774 to 775 MiB |
| Calibrated estimate after the fixtures | 15.4 bytes per pixel (with the 1.15 margin) |
| R5 16-bit TIFF (215 MB): write and verification in the worker / coordinator re-hash | 14.9 to 18.1 s / 0.38 to 0.45 s (whole job 18.4 s) |
| 200 synthetic DNGs (64 x 48 to 462 x 48) | 2.6 to 3.0 s run alone (10 s beside the fixture tests); coordinator private bytes 6.5 to 6.9 MB after 25 jobs, peak 8.5 to 9.6 MB, 8.2 to 9.5 MB at the end; two worker processes for all 200 jobs |

- The coordinator keeps runtime state (parsed documents, folder name caches, attention, session
  timing) only for unfinished batches; a finished batch's state is dropped, and status, reports and
  the output folder of a finished batch are parsed for the caller without being cached, so memory
  does not grow with the number of batches (every single export is one). Recovery scans every
  batch once at start-up (one catalog read per batch, on the coordinator thread).
  While no batch has queued jobs the coordinator polls the journal every 2 s instead of every
  250 ms; submissions, commands and finished attempts wake it at once.
- `tools\package.ps1 -StageOnly` (release build) stages, audits and smoke-runs the layout with
  `lucent-worker.exe`; the staged Lucent.exe, run with a Windows-only PATH, exported the Nikon Z 6
  fixture through the staged worker (`--export-all`, 3.5 s).
- Lucent survives a decoder or encoder crash during exports; a killed or hung worker costs one
  retry. A killed coordinator costs at most the interrupted attempt; the recovery tests kill a
  coordinator process at every publication step (and during an overwrite) and find exactly one
  receipt per job, each matching its file, no duplicate or unrecorded output, no temporary files and
  the originals unchanged. Further recovery tests take the output folder away between the kill and
  the restart (after the rename, after Published, after the receipt: nothing is decided until it is
  back, then the batch ends with one output and one receipt per photo and no re-render of a verified
  output), hold the published output exclusively at start-up (it is completed once it can be read),
  and kill at Complete (the report files are written at the next start); a lease that outlives a step
  is requeued as an interruption and the job fails after `max_interruptions`.
- Every export, single or batch, is now a catalog batch: the batch list grows with every single
  export (phase 3's interface should list or filter them), and exports require the persistent
  library for their durability.
- A coordinator crash in the milliseconds between the rename and the Published step costs a
  re-render (the unrecorded output is removed rather than adopted). A file that appears at the final
  name between verification and rename costs a re-render too.
- Recovery verifies published outputs by hashing them; a 215 MB TIFF takes about half a second.
- The worker re-hashes its output and the coordinator re-hashes it again through the held handle;
  for TIFF that is about 2 % of the export time.
- Pause waits for running attempts, up to the length of one export (about 20 s for an R5 TIFF).
- Destination attentions live in memory: after a restart the batch is just Paused (giving a job back
  records no failure, so nothing in the journal says why); resume re-checks the destination and
  raises the attention again while the problem lasts. Phase 3's interface should word a paused batch
  without an attention neutrally.
- The aggregate memory budget is enforced by admission, not by the operating system: each worker
  process's Job Object limit is the whole worker budget (decision 2), and a second worker is
  admitted only when the running jobs' estimates (or larger observed commits) plus the next fit. A
  job whose real commit exceeds its calibrated estimate (an unusual file, or a calibration from
  small photos) can therefore take two workers together up to twice the worker budget (8.9 GiB
  against 5.95 GiB on the 11.9 GB machine) before either limit stops it. A nested parent Job Object
  with `JOB_OBJECT_LIMIT_JOB_MEMORY` would make the aggregate a hard limit but turns one job's
  overrun into the other job's failure; this is measured on real 45 MP corpora and decided with the
  release performance report (W12, from the W7 closing review, DUR-8). (Corrected by the W12
  amendment below: the worker budget is now a hard aggregate limit, and a job stopped by it beside
  another worker is retried alone instead of failing.)
- A recovery that waits for an unreachable output folder keeps the interrupted job leased: the batch
  is not finished (and its report not written) until the folder is back or the user chooses Use
  this folder.
- Needs for other modules: a per-job thread limit in the worker protocol (decision 2); the
  exporter's own `write_verified` still reopens its temporary file by name inside the worker's
  private scratch folder (covered by the worker's comparison and both hashes, but a handle-held
  writer would remove the window); the roadmap row for W7.

## Alternatives considered

- **Batches inside Lucent.exe on the task executor (no workers).** Rejected by the blueprint
  (section 4): a decoder crash would end the application and native allocations would accumulate.
- **One worker per CPU core, or a fixed pool of two.** Rejected: a 45 MP export peaks at 0.7 to
  0.8 GiB of commit; admission by measured memory, not core count, is the blueprint's rule.
- **A coordinator service process separate from Lucent.exe.** More robust against UI crashes, but a
  second long-lived process, its own IPC to the UI and installation work; the journal already makes
  a UI crash recoverable. Revisit if batches must run without the UI.
- **Publishing with MoveFileEx by name after verifying by name.** The ADR 0006 gap: another process
  could replace the temporary file between verification and the move. Renaming the held handle
  publishes exactly the verified file object.
- **A placeholder file to reserve the final name.** Visible to the user and to imports, and left
  behind by a crash; the no-clobber rename plus the in-memory reservation and a re-check before
  Verified give the same guarantee.
- **Adopting an unrecorded output found after a crash instead of removing it.** The journal cannot
  record Published without the lost lease, so adoption would leave a published file without a
  receipt or need a forged journal; removing it and re-rendering keeps every published file covered
  by a receipt.
- **Failing every remaining job when the destination fills or disappears.** The blueprint wants
  actionable states; pausing with an attention keeps the remaining jobs intact and lets the user
  free space or reconnect the drive and resume.
- **Keeping the W3 in-process ExportService for single exports.** Two integrity paths would differ in
  verification, names and receipts; a batch of one costs about 0.1 s of worker start.
- **Storing the frozen settings document in each job.** The catalog keeps immutable revisions; the
  job stores the revision and its hash, and the as-shot document is a pure function of the build.

## Amendment: aggregate worker memory limit (W12, 2026-10-09)

### Context

The W7 closing review (DUR-8) left the aggregate budget to admission: every worker's own Job Object
limit is the whole worker budget, so two workers whose real commit exceeds their estimates can
together reach twice the budget before either limit stops them, and the blueprint asks for "an
aggregate memory budget". The open question was how to make the sum a hard limit without letting
one job's overrun fail another job.

### Decision

17. **One group Job Object over all workers** (`workers::WorkerGroup`, `worker_group.hpp`). The
    service creates a group whose Job Object has `JOB_OBJECT_LIMIT_JOB_MEMORY` = the worker budget
    (4.45 GiB on the 11.9 GB machine) and `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; every `WorkerHost`
    of the service (`WorkerHostOptions::group`) assigns each worker it starts, still suspended, to
    the group's Job Object first and then to the worker's own Job Object, which therefore nests
    inside it (nested Job Objects, Windows 8 and later). Each worker keeps its per-process limit
    (the worker budget, decision 2) and the engine's refusal threshold: a photo that needs more
    than the budget still fails up front with LimitExceeded. Every allocation that would take the
    workers' total commit beyond the budget fails inside the worker that makes it. A worker that
    cannot join the group (an environment whose own Job Object refuses nesting) runs with its own
    limit only (`WorkerInfo::grouped` false, logged once); a group that cannot be created leaves
    the service with per-process limits (`BatchSchedulerStats::aggregate_limit_bytes` 0).
18. **Classification: transient beside others, deterministic alone** (`worker_host.hpp`, "Failures
    and classes"). Which worker the limit stops is whichever allocates while the group is at its
    limit, not necessarily the one that overran its estimate. The host recognises a group-limit
    stop from the group's `JOB_OBJECT_MSG_JOB_MEMORY_LIMIT` naming its process (delivered in every
    test run) or, because delivery is not guaranteed, from an OutOfMemory or a crash while the
    group's peak commit rose to within 5 % of the limit during the job. When another worker of the
    group was alive at any time during the job, the failure is Transient,
    `WorkerFault::GroupMemoryLimit` (8, appended), OutOfMemory at
    "workers.worker.group_memory_limit": alone the photo may well fit. When the worker was the
    group's only process, the photo needs more than the whole budget: Deterministic,
    `WorkerFault::MemoryLimit`, LimitExceeded at "workers.worker.memory_limit", as for its own
    limit. The worker's own limit takes precedence; either kind recycles the worker (the order of
    the evidence was corrected by the closing review: decision 21). The failure
    takes the ordinary path of decision 5: the temporary output is removed, the attempt's
    publication is closed (Abandoned), the failure is recorded under the lease and the job is
    requeued with its back-off, so the journal never holds a half-published attempt.
19. **The stopped job's retry runs alone; idle workers make room.** A Transient group-limit failure
    marks its job exclusive (in memory, consumed by its next attempt;
    `BatchSchedulerStats::aggregate_limit_hits`): that attempt is admitted only when no other
    attempt runs, and reserves the whole worker budget while it runs, so nothing is admitted beside
    it (`exclusive_attempts`). An idle worker keeps a few MiB to a few hundred MiB of commit between
    jobs (`WorkerInfo::idle_commit_bytes`, measured after each job), which counts against the
    group's limit: admission now counts other slots' idle workers too, and when they are what stands
    between a job and the budget (always, for an exclusive attempt) the coordinator asks their slot
    threads to shut them down and admits the job once they are gone (`idle_workers_released`; a slot
    thread owns its host, so the release is a request it serves while idle). A free slot whose
    worker is running is preferred, so idle workers are reused rather than released when they can
    be. If the exclusive attempt is stopped again while alone, the photo needs more than the budget
    and fails deterministically (18); should it be stopped beside a worker again, the transient
    retry budget (decision 5) bounds the retries (the stop's own retry no longer depends on that
    budget: decision 22).
20. **Test hooks and seams.** The worker test hook `hold-allocation:<ms>` keeps the allocation of
    `allocate:<bytes>` for a while (cancellable), so two workers hold memory at once;
    `BatchTestHooks::admit_before_calibration` admits a second worker before any job was measured.

### Measurements

Development machine (Ryzen 5 5500U, 11.9 GB, idle), 2026-10-09, `dev` build of this amendment.

| What | Result |
|---|---|
| Two real workers holding 600 MiB each in a 1 GiB group (`WorkerGroup.TwoWorkersAboveTheAggregateLimitStopOneOfThemTransiently`) | the second to allocate is stopped (notification delivered; Transient, GroupMemoryLimit; output removed; worker recycled); the holder's job is untouched (604 MiB peak commit); the stopped job, run again alone, gets past its allocation |
| One worker allocating 600 MiB in a 1 GiB group | not stopped; group peak 604 MiB; idle commit after the job 3 MiB |
| One worker alone allocating 700 MiB in a 512 MiB group (its own limit 2 GiB) | Deterministic, MemoryLimit, LimitExceeded ("alone exceeded the workers' aggregate memory limit") |
| A batch of two synthetic photos, two workers each holding 600 MiB under a 1 GiB worker budget (`BatchFailureTest.TwoWorkersAboveTheAggregateLimitRetryTheStoppedPhotoAlone`) | one attempt stopped (one transient failure, its publication Abandoned), retried alone (one exclusive attempt); both photos exported once with one receipt each, nothing left in the folder; 6.4 s |
| 5 smoke fixtures exported to JPEG, two workers (`BatchFixtures.SmokeFixturesExportThroughWorkers`) | the workers together peak at 1040 MiB of the 4559 MiB limit (largest worker 757 MiB); never stopped; 12.6 s |
| 5 smoke fixtures, automatic batch, two workers (`SmokeFixturesAutomaticBatch`) | together 1407 MiB of 4559 MiB (largest worker 1187 MiB, the R5's analysis; 2619 MiB before W12's final check by bands, ADR 0013); never stopped; 26.2 s |
| Canon EOS R5 to 16-bit Display P3 TIFF | worker peak commit 775 MiB |

### Consequences

- The blueprint's aggregate budget is a limit Windows enforces, not only an admission rule: the
  workers together never commit more than the worker budget, and within one session a photo whose
  real commit exceeds its estimate costs at most one retry, alone, never another photo's failure
  (decisions 21 and 22). After a restart its exclusivity is gone (it lives in memory), so each
  restart can cost one more stop and one more retry alone.
- Windows charges an allocation before it refuses it at the limit, so the Job Object peaks
  (`WorkerGroupStats::peak_commit_bytes`, `BatchSchedulerStats::peak_aggregate_commit_bytes`, a
  worker's `JobMetrics::peak_commit_bytes`) include a refused allocation's charge: after a stop the
  group's peak exceeds the limit by the refused allocation (1208 MiB in a 1 GiB group) although
  the processes never held it. Without a stop they are the real peaks (the fixture rows above).
- An exclusive retry waits for the running attempts to finish and costs a worker start for each
  released idle worker (about 0.1 s each).
- Exclusivity lives in memory: after a restart, a job stopped by the group limit is admitted
  normally; if it is stopped beside another worker again, it is marked and retried alone again
  (decision 22). Persisting the mark needs a catalog schema version and is deferred.
- Without notifications, the group's 5 % heuristic can attribute an OutOfMemory or a crash to the
  group's limit when the job failed near it for another reason (retried alone, then judged alone);
  the worker's own limit is no longer inferred from a peak within 5 % of it (decision 21).

### Closing review (W12 part 1, 2026-10-09)

The review found that a stop by the group's limit beside another worker was taken for the photo's
own permanent failure when the worker's own commit plus the refused allocation reached 95 % of its
own limit (COR-1): in production a worker's own limit equals the group's, the own-limit fallback
(a peak within 5 % of it) was consulted before the group's notification, and Windows charges a
refused allocation to the worker's peak. Reproduced with a 1024 MiB group: a holder of 100 MiB,
then 980 MiB in a second worker gave "the worker exceeded its memory limit" (Deterministic), its
peak 984 MiB, the group's notification delivered; alone the allocation fits. Also: a refused
allocation that surfaced under another code than OutOfMemory (std::thread's std::system_error and
other exceptions are Internal; a library's NULL its caller's error) kept the job's own
deterministic class although the group's notification named the worker (COR-3); and a stopped job
whose earlier transient failures had used up the retry budget failed for good instead of getting
its retry alone (COR-4).

21. **The evidence of a memory limit, in order** (`worker_host.cpp`, `memory_failure()`): the
    worker's own Job Object notification (its own limit); the group's notification naming the worker
    (the group's limit, classified by company as decision 18 says, whatever the worker's peak); then,
    only for an OutOfMemory or a worker that ended (notifications are not guaranteed), a peak commit
    above the worker's own limit (a refusal by its own limit always leaves the charged peak above it,
    one by the group's below it whenever the worker's own limit would have admitted the
    allocation), then the group's peak rising to within 5 % of its limit. A notification classifies
    any failure the worker reports except a cancellation or a time-out; the peak fallbacks need an
    OutOfMemory. Regression tests: `WorkerGroup.AStopBesideAnotherWorkerStaysTransientNearTheWorkersOwnLimit`
    (the review's 100 MiB + 980 MiB case: GroupMemoryLimit, Transient; the second worker's peak 984
    MiB; alone the allocation passes) and
    `WorkerGroup.AnAllocationRefusedUnderAnotherErrorCodeIsStillTheGroupsStop` (the worker test hook
    `allocation-error-internal` reports the refused allocation as Internal: GroupMemoryLimit,
    Transient).
22. **A stopped photo always gets its retry alone** (`batch_scheduler.cpp`, `Attempt::fail_job()`):
    a job marked exclusive by a group-limit stop is requeued even when its transient failures have
    used up `BatchRetryPolicy::max_transient_failures`; the mark is consumed by that attempt, so a
    later failure is budgeted again. Regression test:
    `BatchFailureTest.AStoppedPhotoIsRetriedAloneWhateverItsRetryBudget` (a budget of 0: both photos
    exported, the stopped one in two attempts).

Deferred (COR-3, part 2): the group's lost-notification fallback compares the group's peak, which
only grows over the service's life and includes refused charges, so after one refusal a later stop
by a smaller allocation without a notification is no longer recognised and keeps the worker's own
class (an OutOfMemory is Transient anyway). Per-job evidence (the members' commit at the failure, or
the Job Object's limit-violation information) is left for later work; notifications were delivered
in every run measured.

## Amendment: lens corrections in job results (W13 closure, 2026-10-10)

W13 made exports correct lenses with the bundled Lensfun database (ADR 0023), which the worker loads
with its first export job. The closing review found that nothing recorded which database a worker
rendered with, and that a recipe asking for corrections the worker could not apply (no database, or a
named profile it does not hold) exported uncorrected with the outcome "exported" and no note
(APL-04, LNS-3): a replayed recipe changed its result silently, which blueprint section 8 forbids.

23. **Protocol version 3.** `RenderSummaryInfo` gains `lens_database` (the database the render used:
    asset id, version and the first 16 hex digits of its manifest's SHA-256, `app::lens_database_text()`;
    empty without one), `lens_corrections` (what it corrected and with which profile,
    `app::lens_corrections_text()`) and `lens_note` (empty, or why corrections the recipe asks for
    were not applied: the plan's `missing_dependency`, with the worker's own database load problem
    appended). Every other message is unchanged; host and worker are the same build, so the version
    moves to 3 at both ends (`kMinProtocolVersion` = `kMaxProtocolVersion` = 3), and a version-2 peer
    is refused at the handshake. Over-long text is truncated, never refused.
24. **Receipts and reports carry the lens.** `ReceiptDetails` gains `lens_database` and
    `lens_corrections` (receipt JSON member `lens`, optional: receipts of earlier builds have none),
    and every report entry of an exported photo gives them (`lens`, JSON report). A non-empty
    `lens_note` adds the review note "Lens corrections the recipe asks for were not applied: ..." and
    a warning in `lucent.log` (the worker has no log of its own), so the photo counts toward "review
    recommended" instead of passing as plainly exported.
25. **The lens database is part of every job's identity.** The batch service's `renderer_version`
    (part of every job identity, blueprint section 7: "renderer and asset versions") ends with
    "lens database <lens_database_text()>" ("none" without one) from `BatchServiceOptions::
    lens_database`, which the application sets from the database its session loaded: an output made
    with another database is never taken for this one's ("already exported" detection).
