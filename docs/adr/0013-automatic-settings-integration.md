# ADR 0013: Automatic settings in the application: where Auto runs, the full-resolution final check, revisions and records, and automatic batches

- **Status:** Accepted (amended 2026-10-04: "Amendment: W8 integration closure" below; amended
  2026-10-05: "Amendment: geometry, masks and chroma denoising" below; amended 2026-10-09: "Amendment:
  the final check by bands (W12)" below; amended 2026-10-10 by the closing review of W13: "Amendment: Auto and
  lens corrections" below)
- **Date:** 2026-10-04
- **Workflow:** W8 application wiring (services and UI), after the W8 core (ADR 0011) and W7 (ADR 0012)

Sections 1-12 describe the integration as it was decided. Where the closing review found them wrong
or incomplete, the text is corrected in place with a note, and the amendment records what changed.

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
   again (corrected by the closing amendment: the run's one decode is reused, a copy of the decoded
   samples being kept through the analysis); its source diagnostics are measured at full resolution (the saturation coverage of the
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
   `app.batch.auto.check`; corrected by the closing amendment: also when the full-resolution baseline
   reference itself is invalid, which first ended as an Internal error); there is never a loop. Native-crop checks are not repeated (the analysis
   ran them on native-resolution pixels); the check judges the global metrics at full resolution.
   It renders the interactive stage one the analysis judged (highlights clipped after demosaicing);
   the export render clips before demosaicing, which differs within two pixels of saturated sites
   only (ADR 0010). A test hook (`AutoRunRequest::fail_final_checks`, the worker's
   `fail-final-check:<n>`) makes the first n rungs fail, so the ladder is tested on any photo.
   (Corrected by the W12 amendment: the analysis' own stage one and the saturation classes of its
   source pass are reused, so neither the decode nor the stage one is kept twice, and the renders
   are measured band by band with the same statistics: items 21-23.)

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
   `estimate_automatic_memory()` (a full-resolution lease; corrected by the closing amendment: the
   lease is turned into retained memory at once, so the run keeps its bytes but not the render
   place), runs (1)-(2) in process and applies (3).
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

11. **Memory.** `estimate_automatic_memory()` is the larger of the analysis' estimate (with the copy
    of the decoded samples the check reuses, since the closing amendment) and the final
    check's (stage one with the full-size coverage map, then stage one kept, the image context at 9
    and the baseline features at 17 bytes per pixel, and one stage two). The batch scheduler
    estimates analyze attempts with `BatchMemoryOptions::default_auto_bytes_per_pixel` (72) until one
    is measured, calibrated apart from export attempts. (Superseded by the W12 amendment, item 24:
    the check keeps the analysis' stage one, the classes and one render, about 37 bytes per pixel;
    the scheduler's default is 40.)

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
- The pre-pass is durable and restartable: an analysis, once recorded, is never repeated. (Corrected:
  the restart test closes the service gracefully in the middle of it; the crash tests that end the
  coordinator process after an analysis was answered, recorded or frozen were added by the closing
  amendment.)
- Cost: an automatic photo is decoded twice (analysis and final check share one decode since the
  closing amendment; the export decodes again) and, with consistency, three times (a second
  analysis); the final check needs about 2.6 GB for a 45 MP photo, so on the 12 GB reference machine
  a second worker is admitted only next to small photos. Re-running Auto adds revisions (each run is a
  decision of its own). (Corrected: the check's dominant cost is its two full-resolution
  `measure_render()` calls, about 3.9 s of a 7.4 s R5 run, not the second decode; the export cannot
  reuse the check's stage one, which is the interactive kind at the as-shot white balance while the
  export renders the Export kind at the settings' white balance (ADR 0010), so that would change
  exported pixels. The speed-ups that keep every decision are listed in the amendment.)
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

## Amendment: W8 integration closure (2026-10-04) - review fixes and Auto speed-ups

### Context

Two closing reviews (data and services; UX and performance, with a scratch harness over the build's
libraries and the real `Lucent.exe` automation) checked the integration against blueprint sections
2, 6 (steps 5-7), 7 and 8, after main's W9 DNG-opcode work (decoded-RAW contract 1.3, ADR 0014) was
merged in. The one high finding: the interactive Auto held the executor's only render place for its
whole run, so while it ran the editor showed nothing for another photo, never refined a settled
white balance and never rendered 100 % (a new photo's first final frame took 7.6 s instead of 0.7 s
next to an R5 Auto). The others concerned anchors, the freeze racing the editor, redo, durability
of the "edited meanwhile" outcome, cancellation, recovery, cost and wording.

### Decision

13. **Auto holds memory, not the render place.** `AutoService` is admitted with its estimate as
    before and then turns the lease into retained memory (`AdmissionLease::retain_only`, the
    mechanism the develop sessions use for their cached stage ones): the budget still counts the
    run's bytes, but the editor's stage-one builds, refines and 100 % views are admitted beside it
    when they fit (2646 MiB for an R5 Auto plus 725 MiB for an R5 refine or 100 % stage one stays
    within 4096 MiB). A claim that does not fit reports the new stage `WaitingForMemory` ("Waiting
    for memory: other photos are being rendered…") and calls `AutoServiceOptions::make_room` once;
    `Lucent.exe` releases the edit sessions' cached full-resolution stage ones there (rebuilt on
    demand), so a 61 MP photo's Auto no longer waits behind its own 100 % view. After the run the
    file is checked again (`SourceChanged`, nothing written, when it changed during the run). The
    service keeps the results of its last `kKeptAutoResults` (16) finished runs only.

14. **One decode; a baseline choice is checked for validity only.** `run_automatic()` checks the
    memory of the whole run before the analysis, keeps a copy of the decoded samples while
    `AutoSession::open()` consumes the original, and hands the copy to the final check: no second
    probe and decode. When the choice is the baseline (the analysis kept the camera's rendering, or
    every searched control is locked) the ladder is that one rung, and `evaluate_candidate()` can
    fail a baseline on numerical validity only: the check renders it once and scans it for
    non-finite samples, an empty render and non-monotone curves (the same outcome by construction;
    its check values are not recorded). A full-resolution baseline reference that is itself invalid
    ends the ladder as a failed check (`check_failed()`, `app.auto.check`, `app.batch.auto.check`),
    not as an Internal error. Progress follows measured cost (the analysis a quarter of the run,
    then steps after the check's source pass, stage one, context, baseline render and measurement,
    and each attempt).

15. **Anchors keep their settings.** In a batch that keeps related shots consistent, a caller-marked
    anchor (`BatchItem::anchor`) is not analysed again after the pre-pass: `anchor_run()` turns its
    pre-pass analysis into one effective attempt of kind `Anchor` ("anchor", final check `not_run`)
    with its starting settings, confidence 1, no diagnostics of the independent choice, and the
    target of the group its look sets as its consistency record. It is frozen like any automatic
    job (one Auto revision with the submission recipe's settings, so its export identity and
    exported recipe are the submission's); the other members are regularised towards its look. The
    report marks it (`AutomaticReport::anchor`), the history names it "Auto batch: kept as anchor".

16. **The freeze goes through the recipe service.** `RecipeService::write_automatic(item, base,
    write)` runs the batch's freeze transaction under the service's lock with `make_current` = the
    item shows the job's starting revision and no gesture is pending (the editor's rule, item 3);
    `apply_automatic()` is the same call over the store. A gesture in progress therefore keeps its
    own base (the automatic revisions are a branch) and no commit can interleave with the write.

17. **A branch says so, durably; redo follows the photographer.** The stores append the note field
    `branch=kept` (`kAutomaticBranchNote`) to every automatic revision written as a branch. Batch
    receipts (also after a restart), the report's review notes and the Analyse-only reason read it
    ("kept in the history (the photo was edited meanwhile, so they were not applied)";
    `AutomaticReport::branch`). `RecipeService::redo()` returns to the child `undo()` last left in
    the session when it is a child of the current revision, otherwise to the newest child that is not
    such a branch, otherwise to the newest child: undoing the photographer's edit and redoing never
    lands on an Auto result the app had decided not to apply.

18. **Nothing is written after a cancellation; recovery and plans degrade gracefully.** The
    attempt checks the batch's stop state after a worker's analysis arrives (new test point
    "answered") and before anything of it is recorded; an anchor's freeze is checked the same way.
    A recorded pre-pass document this build cannot read (another document version, after an update)
    leaves its job out of the related-shot plan (it is developed on its own) instead of failing the
    plan. Recovery no longer defers an interrupted job of an Analyse-only batch while its folder is
    out of reach: such a batch publishes nothing there but its report.

19. **Wording.** `describe_auto_flag(kFlagLocksPreventedTargets)` is the generic "Locked controls
    kept Auto from its usual targets." (the specifics are the locks report's, listed once); a
    baseline fallback is worded as such in the panel's review box, which the fallback line no longer
    repeats (it says why, as plain text that keyboard and screen-reader users reach); "every control
    is locked" (`kEveryControlLockedNote`) is information, not a lock that prevented something; a
    revision that changed no control shows no confidence; the library card's flag line no longer
    repeats the Review badge; the HTML report's details cell has no stray line break; the Analyse-
    only dialog is titled "Analyse batch", asks for a report folder and hides the export settings.

### Measurements (closure)

Reference laptop, `dev` (RelWithDebInfo), idle machine, 2026-10-04; "before" is HEAD 1d34103's
build, "after" this closure's, alternated on the same files.

| Run | Before | After |
|---|---|---|
| `run_automatic`, Canon EOS R5 (+0.77 EV, confidence 0.90 in both), 3 alternated pairs | 7.33-7.44 s (check 5.44-5.58 s) | 6.82-6.93 s (check 4.93-5.04 s) |
| `run_automatic`, Nikon Z 6 kept as shot | 3.72 s (check 2.19 s), peak working set 1442 MB | 1.89 s (check 0.38 s), 574 MB |
| `Lucent.exe --auto`, R5 | 8.3 s | 7.3 s |
| `Lucent.exe --auto`, R5, every control locked | 5.1 s | 2.65 s |
| A new photo's first final frame (Nikon Z 6) opened 1.5 s into an R5 Auto | 7.65 s (Auto: 9.1 s) | 0.90 s (Auto: 7.3 s; idle 0.69 s) |
| Sony A7R V (61 MP) Auto started while the photo is shown at 100 % | waited until the 100 % stage one was released (20 s in the harness, never in the app while at 100 %) | starts at once (the stage one is released and rebuilt on demand) |

Every decision is unchanged (same choices, confidences and checks); the speed-ups only remove work
whose outcome was already determined.

### Review findings and their resolution

| Finding | Severity | Resolution |
|---|---|---|
| AUX-01 Auto holds the render place | high | Fixed: item 13; tests `TheRunHoldsItsMemoryButNotTheRenderPlace` and the harness numbers above |
| AUT-1 anchors re-optimised | medium | Fixed: item 15; `AnAnchorKeepsItsSettingsAndSetsItsGroupsLook` |
| AUT-2 freeze behind the recipe service | medium | Fixed: item 16; `AGestureInProgressKeepsItsBaseWhenAutomaticRevisionsArrive`, `APhotoEditedDuringTheBatchKeepsItsEditAndTheResultIsABranch` |
| AUT-3, AUX-05 redo lands on the branch | medium, low | Fixed: item 17; `RedoReturnsToThePhotographersEditNotToAnAutomaticBranch` (in session and after a restart) |
| AUX-02 waiting on a 100 % view; refused above about 69 MP | medium | The wait is fixed (item 13, `ARunWaitingForMemoryAsksForRoomOnce`). The ceiling is deferred to W12: the check's full-resolution context and baseline features (26 bytes per pixel, the analysis library's `measure_render()` contract) need a banded or reduced-precision measurement, a metric-contract change |
| AUX-03 the check re-decodes and rebuilds the stage one | medium | The second probe and decode are removed (item 14, about 0.5 s of 7.4 s on the R5). Keeping the analysis' own stage one, folding the upright grid into its source pass and skipping local contrast in the check's measurements need options inside `lucent_analysis` (`AutoSession`, `measure_render`) and are deferred with the `measure_render()` speed-up (52 % of the run, a metric-contract version) to the W12 performance work |
| AUX-04 a no-change result pays the full check | medium | Fixed: item 14 (validity scan) and item 19 (no confidence without a change, "everything locked" as information); the revision is still written: it is the run's record (its locks report and review), and the history names it "Auto" with no change |
| AUT-4 the branch outcome is not stored | low | Fixed: item 17 |
| AUT-5 no crash test for the pre-pass or the freeze | low | Fixed: `lucent_batch_test_coordinator` gained automatic and related-shot modes; `AutoRecovery` ends it at "answered" (independent and consistent), "analysed" and "frozen" and checks one freeze per photo, recorded analyses unchanged and receipts bound to the effective revisions |
| AUT-6 an invalid baseline is an Internal error | low | Fixed: item 14; `AnInvalidBaselineFailsTheCheckNotTheRun` (test hook `AutoRunOptions::test_invalid_baseline`) |
| AUT-7 identity checked only before the run | low | Fixed: item 13; `AFileThatChangesDuringTheRunGetsNoRevision` |
| AUT-8 locks report covers brightness only | low | Deferred to the Natural v1 calibration (ADR 0011, pending part of W8): the analysis makes no white-balance proposal when white balance is locked (`AutoResult::white_balance` is for unlocked white balance), so a prevented correction cannot be judged without an analysis change; the brightness findings are documented as measured on the optimiser's choice |
| AUT-9 the pre-pass needs an exact document version | low | Fixed: item 18; `AnAnalysisThisBuildCannotReadIsLeftOutOfThePlan` |
| AUT-10 a cancellation does not stop the freeze | low | Fixed: item 18; `NothingIsWrittenAfterACancellation` |
| AUT-11 recovery holds Analyse-only jobs back | low | Fixed: item 18; `AutoRecoveryAnalyzeOnly.IsNotHeldBackByAnUnreachableReportFolder` |
| AUX-06 duplicate or conflicting review wording | low | Fixed: item 19; conversion, model and QML tests |
| AUX-07 Analyse only asks for export settings | low | Fixed: item 19; model and QML tests |
| AUX-08 progress not proportional to time | low | Fixed: item 14 (R5: 25 % after 1.9 s, 50 % after 3.3 s of 6.8 s) |
| AUX-09 every result kept for the process' life | low | Fixed: item 13; `OnlyTheLastResultsAreKept` |
| AUX-10 two progress bars; hover-only details | low | Fixed: the progress bar alone carries the stage as its name; the failed checks are plain text |
| (new) `WorkerServer.MemoryCountersDescribeThisProcess` failed once (peak working set 4 KiB below the current one) | low | Fixed: `GetProcessMemoryInfo` does not read its counters atomically; `current_process_memory()` never reports a peak below the current value |
| AUX-11 documentation inaccuracies | low | Fixed: Consequences and items 2, 7 and 11 corrected in place; `edit_session.hpp`, `auto_settings_model.hpp`, `presentation.hpp` and the plan's cost note ("for large photos about twice as long as their export") |

### Consequences (closure)

- Interactive editing stays usable while Auto runs: only memory is shared, and the budget still
  bounds the sum.
- The decode copy costs the raw frame's bytes during the analysis (about 90 MB for 45 MP), counted
  by `estimate_automatic_memory()`; it never raises the R5's estimate (the check's phase dominates).
- An anchor's revision is a record of its role, not a decision: its confidence is 1 and it is never
  checked, as the photographer's own look is exported unchanged.
- Remaining costs, deferred to W12: `measure_render()` (two calls, about 3.9 s of the R5's run), the
  check's stage one and upright source pass that the analysis already computed, and the check's
  26 bytes per pixel that refuse Auto above about 69 MP (GFX100-class files) on the 4 GiB budget.
  (Resolved by the W12 amendment "the final check by bands" below.)

## Amendment: geometry, masks and chroma denoising (W9 part 2, 2026-10-05)

DevelopSettings v2 (ADR 0015) adds the photographer's geometry (crop, straighten, quarter turns,
flip), masks and chroma denoising. Their interplay with the automatic settings (ADR 0015,
"Amendment: phase 2 of 4", decision 5):

- **Decision 1.** Auto adjusts global controls only. `AutoSession::open` evaluates the current
  settings without their geometry and local adjustments (`analysis::evaluation_settings()`):
  native crops are stage-one sub-rectangles, so a crop or rotation would not line up with them and a
  mask would land on other content; and the decision is then the same whatever the crop. Chroma
  denoising is global and stays in the evaluation. Every settings value the analysis returns carries
  the current geometry, masks and denoising unchanged, so the revisions Auto writes keep them (their
  notes never list a part). An anchor's appearance is measured the same way.
- **Decision 2.** The final check renders the full settings (geometry and masks included) for every
  rung and for the baseline, which carries them too, and measures them in the geometry's output
  frame: the image context and the neutral-surface mask are mapped through the same
  `GeometryMapping` (`analysis::map_image_context()`). Its memory estimate uses the current settings
  (`estimate_automatic_memory(metadata, current, options)`).
- No new locks: Auto never changes the parts, so there is nothing to lock.

## Amendment: the final check by bands (W12, 2026-10-09) - AUX-02 and AUX-03

### Context

The W8 closure deferred two costs of the full-resolution final check (item 2) to W12. AUX-03: the
check rendered a second stage one, from a copy of the decoded samples kept through the analysis,
although the analysis had rendered exactly that stage one, and ran a second source pass for the
full grid's saturation coverage. AUX-02: the measurement contract of `measure_render()` kept
per-pixel planes of the whole image, the context (9 bytes per pixel) and the baseline's features
(17), plus its own temporaries (the log luminance and two box planes of local contrast, 12): the
check peaked at about 61 bytes per pixel (2619 MiB for the 45 MP R5) and Auto was refused above
about 69 MP on the 4 GiB interactive budget (GFX100-class files).

### Decision

21. **The analysis' stage one is the check's** (`analysis::AutoOptions::keep_full_resolution`,
    `AutoSession::take_full_resolution()`; ADR 0011, amendment "Full-resolution measurements by
    bands"). The check's stage one was always the analysis' request (Interactive, as shot, full
    resolution): the settings' white balance and every other control are applied in stage two, so
    the analysis' own stage one can be reused for every run that has a final check. The camera image
    area the evaluation set aside is restored on it, and the saturation classes of its upright grid
    (one byte per pixel; `analysis::SaturationClasses`) come from the analysis' source pass, as does
    the noise model. No decoded copy is kept, no second source pass or stage one is made. Runs without
    the check (the related-shot pre-pass, anchors) keep nothing.
22. **The check measures by bands** (`analysis::FrameMeasurement`). Each rung is rendered whole
    (the engine renders whole images) and measured against the baseline band by band: the context
    of a band of output rows (signal from the stage one, saturation from the classes, the neutral
    surfaces from the white-balance evidence's per-pixel rule, all through the rung's geometry) and
    the baseline's features of that band exist only while the band is measured. When the baseline's
    stage two is per pixel and its geometry only crops (no local tone, chroma denoising or masks;
    no straightening, quarter turn or flip: the as-shot baseline of an unedited photo), the
    baseline is re-rendered band by band from the stage one; otherwise its full render is kept
    (12 bytes per pixel). The baseline's own statistics come from the same pass as the first rung's.
    Every statistic is bit-identical to the whole-image measurement (proved by
    `FrameMeasurement.BandsMeasureExactlyAsTheWholeImage` for identity, crop, camera-area, straighten,
    quarter-turn, local-tone, denoised and context-free baselines, and on the fixtures below); local
    contrast is not measured (neither `evaluate_candidate()`'s feasibility nor the recorded check
    values used it). The ladder is unchanged: its rungs, order, reasons, failures and values; a
    numerically invalid baseline still ends it as a failed check (its statistics are known before any
    rung is judged; when the first rung cannot be rendered they are measured on their own).
23. **The baseline rung of a choice that is the baseline consumes the stage one** (its render is the
    stage one's last use: stage two then works in place).
24. **Memory.** `estimate_automatic_memory()` is the larger of the analysis' estimate (keeping its
    stage one and one byte per pixel of classes) and the check's: the stage one and the classes,
    the baseline's full render when it is not streamed, and either one stage two of an attempt that
    may add local tone (unless shadows and highlights are both locked) or a render with one
    measurement band (`analysis::frame_measurement_band_bytes()`: about 50 bytes per pixel of one
    band, 384 rows on this 12-thread machine and never more than 4 Mi pixels). For a colour photo that is about 37 bytes per pixel (it was
    about 62, and the old formula underestimated the R5's measured peak). The batch scheduler's
    estimate of analysis attempts before it measured one
    (`BatchMemoryOptions::default_auto_bytes_per_pixel`) is 40 (was 72).
25. **Progress** follows the new costs: the analysis is about a third of a 45 MP run with the
    check; the first rung's band measurement reports per band.

### Measurements

Development machine (Ryzen 5 5500U, 11.9 GB, 12 threads, idle), 2026-10-09; "before" is a build of
HEAD e51dd65 with the same measurement harness (`AutoMeasurement.DISABLED_OneFile`,
`run_automatic` in process from as-shot settings, one process per run), alternated with "after",
three rounds each.

| Photo | Before | After |
|---|---|---|
| Canon EOS R5 (45 MP, +0.77 EV): total / check | 7.44-8.47 s (median 7.63) / 5.50-6.41 s | 5.53-5.82 s (median 5.61) / 3.52-3.73 s |
| Canon EOS R5: peak commit / estimate | 2619 MiB / 2134 MiB | 1187-1188 MiB / 1579 MiB |
| Sony A7R V (61 MP, kept as shot: render and validity scan): total / check | 3.04-3.07 s / 1.23-1.25 s | 2.71-2.76 s / 0.85-0.91 s |
| Sony A7R V: peak commit / estimate | 2095-2098 MiB / 3597 MiB | 1395-1398 MiB / 2153 MiB |
| Fujifilm X-T4 (26 MP, X-Trans): total / peak commit | 8.03-8.29 s / 1539-1542 MiB | 5.48-6.13 s / 948-956 MiB |
| Synthetic 12000 x 8400 DNG (100.8 MP, +0.28 EV; `AutoLargePhoto.AHundredMegapixelPhotoRunsWithinTheBudget`) | refused (estimate above 4 GiB) | 10.8-11.4 s (analysis 2.5-2.7 s, check 8.3-8.7 s; 17.3-19.5 s in an earlier session of this machine); estimate 3557 MiB of 4096; peak commit 2550-2578 MiB |
| Automatic batch of the 5 smoke fixtures, two workers: largest worker commit | 2619 MiB (W8, item 11's table) | 1187 MiB |

Decisions are identical: the "lucent.auto-run" documents of all five smoke fixtures (R5, A7 IV,
DJI FC4382, X-T4, Z 6) and the A7R V, before and after, are equal in everything but timings and
the memory estimate (chosen settings, confidence and its breakdown, flags with reasons,
diagnostics, attempts with their outcomes, failures and full-resolution check values); the R5 and
X-T4 ran the full check (render and band measurement), the others the baseline's validity scan.

### Review findings and their resolution

| Finding | Resolution |
|---|---|
| AUX-02 Auto refused above about 69 MP | Fixed: items 21-24. The check holds no per-pixel plane but the stage one, the classes and one render (plus the baseline's render when it cannot be streamed); Auto now fits about 116 MP on the 4 GiB interactive budget (126 MP on this machine's 4.45 GiB worker budget) by the estimate, about 160 MP by the measured peaks |
| AUX-03 the check re-renders a stage one the analysis had | Fixed: item 21 (also no second source pass, no decoded copy); with item 22 the R5's check is 35 % shorter |

### Consequences (W12)

- The check still runs two kernel passes over the image (the baseline's and the choice's) and the
  baseline's band renders, so its time grows with the pixels (about 8.5 s of check for 100 MP).
- A photo whose baseline cannot be streamed (the photographer locked local tone, denoised, masked,
  straightened or turned it) keeps the baseline's render: 12 bytes per pixel more, which the
  estimate counts.
- The estimate stays conservative (it assumes the search may add local tone); the measured peaks
  are about 26 bytes per pixel.
- The closure's remaining costs (its Consequences: `measure_render()`, the check's stage one and
  source pass, the 26 bytes per pixel) are resolved; local contrast at full resolution is no longer
  computed, since nothing in the check used it.

## Amendment: Auto and lens corrections (W13 closure, 2026-10-10) - APL-10

ADR 0010's W13 amendment keeps Auto's analysis and final check on the uncorrected image (native crops
and band renders are parts of the frame, which stage two does not correct; a corrected candidate
would be compared with uncorrected bands). The closing review asked that the consequence not stay
silent: blueprint section 6, step 7 renders the selected recipe once at full resolution and applies
the final checks to it, while an exact lens match's export also applies the profile's vignetting gain
(2.3 to 3 at the corners wide open in the corpus), which can push bright corners past the output's
white after Auto's check passed.

- **Deviation recorded.** For files whose lens is corrected, Auto's final check is not a check of the
  exported image: it measures the same recipe before the lens correction. Running the check on a
  corrected whole-frame render was rejected for now: the bands that keep Auto under its memory budget
  (W12 amendment above) would each need the correction's point mapping and a halo of source rows,
  and the baseline's streamed bands would have to be corrected identically.
- **Mitigation in batches.** An automatic export whose lens corrections include vignetting and whose
  render maps more than 0.1 % of its pixels to the output's white gets the review note "Automatic
  settings were checked before lens corrections; this export's vignetting correction brightens its
  corners, and N % of its pixels are brighter than the output can show: check the corners."
  (`Attempt::review_notes()`, a tenth of the general 1 % threshold that flags any export). The
  editor shows the corrected frame after Auto, so the photographer sees the corners there.
- Revisit together with ADR 0010's "Auto never changes the lens settings" item, when the analysis
  learns lens corrections.
