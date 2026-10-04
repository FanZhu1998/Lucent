# ADR 0013: Automatic settings in the application: where Auto runs, the full-resolution final check, revisions and records, and automatic batches

- **Status:** Accepted
- **Date:** 2026-10-04
- **Workflow:** W8 application wiring (services), after the W8 core (ADR 0011) and W7 (ADR 0012)

## Context

ADR 0011 delivered Natural v1 as a library (`lucent_analysis`): the per-image controller, the
related-shot session, grouping and targets, and the analysis-resolution recheck with its bounded
conservative fallback. It left the application's part open and recorded what that part needs:
an automatic revision origin and a record of policy and search versions per automatic revision;
the job journal persisting `AutoResult`'s flags, confidence and versions; the editor's "Auto" running
the analysis with the item's current settings, locks included; `ConsistencyRecord`, shot features and
grouping persisted with each item's result; and (item 22) the final full-resolution check: "render
the chosen settings and the baseline once at full resolution, `make_image_context()`,
`measure_render()` and `evaluate_candidate()` with the same metric contract, and walk
`conservative_fallbacks()` on failure, recording each attempt as a new revision with its reason".
Item 28 asks a worker that replays results across machines to disable the CRT's FMA3 code paths.

The blueprint fixes the behaviour:

- Section 2, "Detailed individual editing": "**Auto** creates a new editable revision. The
  photographer can lock controls before rerunning it. The optimizer may change only unlocked controls
  and must report when the locks prevent its usual technical targets." "Editing a picture during a
  batch creates a new revision; an already queued export retains its frozen revision until the user
  explicitly replaces or requeues it."
- Section 2, "Automatic batch development": "Choose Natural rendering ... and whether related
  photographs should be kept visually consistent", "Lucent analyzes each image, chooses its own
  settings, validates the result, and exports automatically", "A low-confidence aesthetic decision
  produces a restrained result and a review flag. An undecodable file or an image missing a mandatory
  interpretation step is skipped with a specific explanation; other files continue. An optional
  **Analyze only** mode produces editable previews before export."
- Section 6 steps 5-7: regularise related shots and revalidate them, "Revalidate each image, then
  freeze the full recipe, policy/search versions ... and diagnostic results"; "Render the selected
  recipe once at full resolution. If final checks fail, try a bounded conservative fallback and
  recheck. Every fallback that changes image operations creates and freezes a new recipe revision,
  retains the previous attempt and reason, and binds export identity/receipt to the effective
  revision. If still invalid, mark the file failed; do not loop indefinitely or block the rest of the
  batch." "Manual locks apply throughout candidate search, group consistency, fallback, and final
  quality retries."
- Section 7: the job lifecycle "Discovered -> Probed -> Analyzed -> Recipe frozen -> Queued ->
  Rendering ...", jobs persisted before dispatch and restartable from the latest valid stage.
- Section 8: an immutable recipe records "user/automatic origin ... policy/search version ... and
  diagnostic results. Avoid a single opaque 'auto enhanced' flag."

## Decision

1. **One shared run** (`application/auto_develop.hpp`, `app::run_automatic`). The interactive "Auto"
   and automatic batches run exactly the same code, so they decide alike: probe, memory check, one
   decode; an Unsupported interpretation fails with Unsupported (`app.auto.support`) unless the
   caller allows it; `analysis::AutoSession::open` from the item's committed settings (locks and
   locked values); with a consistency target `regularise(target)`; for an anchor
   `appearance_of(current)`; the session is released; then the full-resolution final check (2) and the
   locks report (5). The result is an `AutoRun`: versions, starting and chosen settings, confidence
   and its breakdown, flags with reasons, compact diagnostics, shot features, the independent
   appearance, the attempts of the check and the effective one.

2. **The full-resolution final check** (blueprint step 7, ADR 0011 item 22). The file is decoded
   again; its source diagnostics are measured at full resolution (the saturation coverage of the
   upright full-size grid, the noise model) before one interactive stage one (as shot) consumes it;
   `make_image_context()` builds the context; the analysis' own policy context is rebuilt from the
   result (the tone proposal, the white-balance evidence with its neutral-surface mask at full
   resolution, the locked exposure); the baseline (`AutoResult::candidates[0]`) is rendered and
   measured as the reference; then the ladder is walked, each rung rendered once and judged by
   `evaluate_candidate()` against that baseline: the chosen settings, at most
   `analysis::kMaxFallbackRechecks` (2) of `conservative_fallbacks(chosen, baseline)`, then the
   baseline itself (as shot for unlocked controls, locked controls kept: locks hold through the
   fallback). The first rung that passes is the effective attempt. The baseline fails only when it is
   numerically invalid: then no attempt is effective and the photo fails (`app.auto.check`,
   `app.batch.auto.check`); there is never a loop. Native-crop checks are not repeated (the analysis
   ran them on native-resolution pixels); the check judges the global metrics at full resolution.
   It renders the interactive stage one the analysis judged (highlights clipped after demosaicing);
   the export render clips before demosaicing, which differs within two pixels of saturated sites
   only (ADR 0010). A test hook (`AutoRunRequest::fail_final_checks`, the worker's
   `fail-final-check:<n>`) makes the first n rungs fail, so the ladder is tested on any photo.

3. **Revisions.** One automatic revision (origin `Auto`, the canonical "lucent.develop" document) per
   attempt up to and including the effective one, each **derived from the revision the run started
   from** (siblings, not a chain): undo of an automatic edit returns to the recipe before it, and a
   failed attempt stays in the history, with its reason, as another branch. Notes are the recipe
   service's key=value fields: `auto=<chosen|fallback|baseline>;attempt=<n>;check=<passed|failed|
   not_run>;policy=natural/<version>;changed=<controls>;reason=<text>`. The effective revision becomes
   the item's current revision only when the item still shows the starting revision (and, in the
   editor, no gesture is pending); otherwise the photographer edited it meanwhile and the revisions
   are kept as a branch (`made_current` false). Nothing is written after a cancellation or an error.
   `RecipeStore::append_automatic` / `append_automatic_revisions` (catalog and memory stores) write the
   revisions and their records atomically; `RecipeService::apply_automatic` is the editor's entry and
   `RecipeService::refresh` reloads an item whose revisions the batch service wrote.

4. **Analysis records** (catalog schema v5, `auto_analyses`; minimum reader version stays 2). One
   immutable record per automatic revision (UNIQUE revision, deleted only with its item): the policy
   and its version, the search, metric-contract, develop-pipeline and develop-settings versions, the
   consistency version when it was regularised, the attempt and its final-check outcome
   (`passed`, `failed`, `not_run`), the confidence (capped at `kFallbackConfidence` for fallbacks),
   whether a review is recommended, the flags with their reasons (a JSON array) and the
   "lucent.auto-record" v1 document (the attempt and its full-resolution measurements, the confidence
   breakdown, compact diagnostics, the consistency target and terms, the locks report, timings). The
   columns are queryable; the documents are the application's. Session libraries keep records in the
   memory store.

5. **Locks.** The run lists the locked controls the optimiser would set and reports explicitly when
   they kept it from its usual targets: a brightness deficit or excess of more than
   `kLockedTargetEv` (0.25 EV, measured on the chosen settings' tonal metric) left with exposure locked
   (or shadows / highlights locked where the tone proposal would have used them), and locked settings
   that clip (`LockedControlsClip`); "everything locked" is noted but is not by itself a missed target.
   A prevented target adds the flag `locks_prevented_targets` (1002), which recommends a review, with
   the user-facing reasons.

6. **Review flags.** The analysis' flags keep their codes; the application adds
   `final_check_fallback` (1001, the full-resolution check failed the choice) and
   `locks_prevented_targets` (1002). Those that recommend a review: low confidence (the restrained
   edit), large edit, conservative fallback (either check), no feasible candidate, rejected recheck,
   a related-shot look not reached, visible shadow noise, locked settings that clip, a limited
   interpretation, and prevented targets. White-balance uncertainty (common: the estimators often
   disagree, ADR 0011), missing evidence and source saturation are reported but do not by themselves
   recommend a review. Each has user-facing wording (`describe_auto_flag`). A low-confidence result
   is exported as the restrained edit with its review notes in the receipt and the report.

7. **Interactive Auto** (`application/auto_service.hpp`). `AutoService::run(item)` queues a task on the
   executor's Background lane (never the UI thread): it reads the committed settings, checks that the
   file is still the library's (SourceChanged), waits for memory admission with
   `estimate_automatic_memory()` (a full-resolution lease), runs (1)-(2) in process and applies (3).
   Progress (analysing, checking, saving) and completion are observable; `cancel()` stops it at the
   next safe point and nothing is written; one run per item at a time (`app.auto.busy`).

8. **Automatic batches: stages in the journal** (catalog schema v5). `BatchRequest::automatic` is
   Off, Auto or AnalyzeOnly (`batches.auto_mode`); every job of an automatic batch carries
   `jobs.auto_state`, moving only forward (a trigger): **Pending** (its revision and settings are the
   recipe frozen at submission, the analysis' starting point; its identity is provisional) ->
   **Analysed** (`jobs.analysis_json` holds the "lucent.auto-run" document, persisted before anything
   is rendered) -> **Frozen**. Freezing is ONE catalog transaction: the automatic revisions and their
   records (3, 4), then `freeze_auto_job`, which rebinds the job's revision, settings SHA-256 and
   identity (recomputed with `compute_job_identity`) to the effective revision; the identity trigger
   allows exactly that one change. Publication refuses any automatic job that is not Frozen, so the
   export identity, the output name's revision number and the receipt are always the effective
   revision's. AnalyzeOnly batches complete a Frozen job without a receipt (`complete_without_output`)
   in the same transaction; their report lists it as Analysed. Without "keep related shots
   consistent", one analyze job runs the analysis and the final check and the same attempt freezes
   and exports. Existing outputs are not looked up at submission for automatic jobs (their identity
   is unknown until frozen). Failures follow ADR 0012: an Unsupported photo is skipped with its
   reason; a final check in which nothing passes fails that photo only.

9. **The related-shot pre-pass** ("keep related shots consistent"). While any job of the batch is
   Pending, the coordinator claims only Pending jobs (`ClaimRequest::auto_states`). A pre-pass attempt
   runs an analyze job without the final check, records the document and gives the job back
   (Analysed, Queued) in one transaction: a restart never analyses it again. Once none is pending,
   the coordinator computes the groups (`analysis::group_related_shots`: capture time, similarity,
   illumination; the user's groups from `BatchItem::group`; anchors from `BatchItem::anchor`) and the
   targets (`analysis::consistency_targets`) from every recorded document, in job order; this is
   deterministic, so a restart computes the same plan, which is cached per batch while the number of
   recorded documents is unchanged. Each Analysed job's second analyze job reopens the session from
   the file (identical by determinism, ADR 0011 item 16), regularises it towards its target (or not:
   alone, anchor, manual) and runs the final check; then it freezes and exports. The pre-pass's
   document stays the job's analysis document; the regularised run's detail is in the records. The
   user's groups and anchors are kept by job ordinal in the policy document ("automatic" member,
   optional on reading).

10. **Where it runs, and the protocol** ("lucent.worker" version 2). Batches analyse in the worker,
    isolated from Lucent.exe: a new `analyze` job (host -> worker) carries the source, the starting
    recipe and the "lucent.auto-request" document (the target, anchor, whether to check); the
    `analysis` answer carries the "lucent.auto-run" document; both documents travel as JSON text in
    a string member, so the strict protocol reader stays independent of the application and its
    nesting limit; the protocol checks their size and UTF-8, the application reads them strictly.
    New progress stages Analysing (6) and Checking (7). Every version-1 message is unchanged;
    `kMinProtocolVersion` and `kMaxProtocolVersion` are both 2 (a version-1 peer cannot analyse).
    `WorkerHost::run_analysis` supervises an analyze job exactly like an export job (heartbeats,
    timeouts, cancel, faults); an analysis naming another source or recipe is a protocol violation.
    The worker disables the CRT's FMA3 paths (`_set_FMA3_enable(0)`, ADR 0011 item 28), so a batch's
    decisions do not depend on the computer; Lucent.exe's in-process Auto keeps the runtime's choice
    (a last-bit difference can only resolve a near-tie differently).

11. **Memory.** `estimate_automatic_memory()` is the larger of the analysis' estimate and the final
    check's (stage one with the full-size coverage map, then stage one kept, the image context at 9
    and the baseline features at 17 bytes per pixel, and one stage two). The batch scheduler
    estimates analyze attempts with `BatchMemoryOptions::default_auto_bytes_per_pixel` (72) until one
    is measured, calibrated apart from export attempts.

12. **Reports.** Every entry of an automatic batch carries an `AutomaticReport`: state, effective
    revision and its number, confidence, the effective attempt and how many revisions the job wrote,
    whether a review is recommended, the flags (user-facing), the related-shot group and what the
    locks did. `ReportOutcome::Analysed` and `BatchReport::analysed` describe AnalyzeOnly batches;
    `BatchStatus::review_recommended` adds analysed photos whose record recommends a review; the JSON
    report adds `summary.analysed` and `entries[].automatic` (members added to version 1).

### Measurements

Reference laptop (Ryzen 5 5500U, 12 GB), `dev` (RelWithDebInfo), 2026-10-04, while another
workflow built in a second worktree.

| Run | Result |
|---|---|
| Interactive Auto, Canon EOS R5 (8191 x 5463), in process | 9.5 s: analysis 2.2 s, full-resolution final check 7.4 s (1 attempt); +0.77 EV, confidence 0.90; estimate 2646 MiB, process peak working set 2620 MiB |
| Automatic batch, 5 smoke fixtures, JPEG, up to 2 workers | 29.0 s for the batch (4 exported, the DJI DNG excluded as unsupported); per photo 8.3-15.7 s from dispatch to receipt (analysis, check, export); peak worker commit 2619 MB (the R5's final check) |
| AnalyzeOnly with related shots, the Nikon Z 6 pair | grouped by capture time ("time-1"), both regularised; no output |

The final check dominates: a second decode, a full-resolution source analysis and stage one, and two
full-resolution stage-two renders and measurements, at about 60 bytes per pixel of peak memory.

## Consequences

- The editor and batches decide alike and every automatic edit is an ordinary, editable, undoable
  revision with a queryable record; nothing about an automatic result is an opaque flag.
- The export identity of an automatic job is the effective revision's by construction: the catalog
  refuses publication before the freeze, and the freeze rebinds the identity once, in the transaction
  that writes the revisions.
- The pre-pass is durable and restartable: an analysis, once recorded, is never repeated; the
  restart test kills the service in the middle of it.
- Cost: an automatic photo is decoded three times (analysis, final check, export) and, with
  consistency, four (a second analysis); the final check needs about 2.6 GB for a 45 MP photo, so on
  the 12 GB reference machine a second worker is admitted only next to small photos. Re-running Auto
  adds revisions (each run is a decision of its own). Speed-ups that keep the decisions: reusing the
  analysis decode for the check (keeping a copy of the decoded samples), skipping the check's
  re-render when the choice is the baseline, and the export reusing the check's stage one; none is
  needed for correctness.
- A Failed job that is retried after the pre-pass joins the related-shot plan late: members frozen
  before it were regularised without it (the plan is recomputed when the document count changes).
- Grouping depends on capture times; synthetic test photos use the user-group override.
- Requests for the UI phase: an "Auto" command calling `AutoService` (progress, cancel, the new
  revision and the run's flags and locks reasons); the batch dialog's Off / Auto / Analyze only and
  "Keep related shots consistent" (`BatchRequest::automatic`, `keep_consistent`, plan notices 7-9);
  analysis progress (worker stages Analysing and Checking, `BatchStatus::automatic_counts`); review
  counts and report entries (`AutomaticReport`). Lucent.exe may call `_set_FMA3_enable(0)` at start
  if in-process and worker decisions must agree to the last bit.

## Alternatives considered

- **The final check inside the export job** (one decode less): the fallback revisions and the
  identity binding must exist before anything is published, and a fallback found during export
  would rename and re-identify an output after the fact; the separate analyze job keeps the
  invariant "frozen before published" checkable by the catalog.
- **Fallback revisions as a chain** (each derived from the failed one): undo would step into the
  failed attempt; siblings keep undo meaningful and still retain every attempt with its reason.
- **Records in revision notes**: notes are free text for the history list; versions, outcomes and
  confidence must be queryable and immutable, so they are columns of their own table.
- **Persisting the related-shot targets** at the end of the pre-pass: recomputing them from the
  recorded documents is deterministic and needs no further state; the cache only saves time.
- **Keeping analysis sessions in the worker** between the pre-pass and the second analysis: not
  durable across restarts, and 50-90 MiB per photo held across a batch; reopening from the file gives
  the same result.
- **Interactive Auto in a worker process**: crash isolation for an in-process analysis of the photo
  being edited is not needed (the editor already renders it in process), and the in-process run
  shares the executor's memory admission with the editor's previews.
