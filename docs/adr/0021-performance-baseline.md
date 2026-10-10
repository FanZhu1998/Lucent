# ADR 0021: Performance baseline: a reproducible harness over the shipped programs, the first baseline, and regression budgets only where the variance allows

- **Status:** Accepted (amended 2026-10-09: "Amendment: closing review of W12 part 1" below)
- **Date:** 2026-10-09
- **Workflow:** W12 (release hardening), the performance report

## Context

Blueprint section 10, "Performance targets to measure", asks Lucent to "Measure cold-start and
warm-start time, time to first RAW preview, median/p95 per-image analysis/export time, peak RAM,
CPU utilization, cancellation latency, and completed images per minute on a documented 16 GB
laptop. These are engineering targets, not demonstrated figures." Its release gate "Batch
resilience" requires that "A 1,000-file mixed corpus completes without unbounded memory growth;
injected corrupt files, worker crashes, disk-full, cancellation, and drive removal do not lose
edits or falsely report completion". Section 7 sets the memory rules the numbers have to respect
(an aggregate budget, workers admitted by measured memory, bounded caches). The roadmap's W12 row
asks for a performance report, which was also to settle two questions carried from earlier
increments; both were decided by this increment's fixes before the measurements here:

- whether worker processes get a hard aggregate memory limit (W7, DUR-8): yes, one group Job Object
  over all workers (ADR 0012, amendment "aggregate worker memory limit");
- the cost of Auto's full-resolution final check (W8, AUX-02 and AUX-03): it reuses the analysis'
  stage one and measures by bands (ADR 0013, amendment "the final check by bands").

Every figure recorded before this ADR was measured once, ad hoc, mostly on `dev` builds
(RelWithDebInfo), often while other workflows built on the same machine (ADRs 0007, 0008, 0011,
0012, 0013), with no repetitions, no statement of the file-cache state and no CPU accounting. They
cannot show a regression, and nothing could tell a real change from noise. The 1,000-file gate had
never been run.

The reference machine is a Ryzen 5 5500U laptop (6 cores, 12 threads, 11.9 GB, NVMe SSD, integrated
Radeon graphics, Windows 11), not the blueprint's 16 GB laptop; on it, Lucent's worker budget is
4.45 GiB (ADR 0012, decision 4). It is also the development machine: during these measurements it
ran the developer's desktop applications (editors and their language servers, a remote-desktop
client, chat clients, antivirus), with about 2 GB of physical memory available before each run.

## Decision

1. **A harness over the shipped programs** (`tools\bench`, documented in `docs\performance\README.md`).
   PowerShell scripts drive the existing executables, each run in its own process: Lucent.exe and
   its developer automation with a throwaway `--data-root` per run (start-up, thumbnails, first RAW
   preview, edit latency, Auto in the editor and its cancellation, batches through lucent-worker.exe
   and their cancellation), lucent-render.exe (one export per photo, in process) and lucent-auto.exe
   (the analysis alone). No library is linked into a separate benchmark program: the numbers are
   those of the programs users run.
   - `LucentBench.psm1` holds `BenchProcess`, a small C# class compiled with Add-Type that starts a
     program with its output captured line by line (each line with its arrival time), samples the
     program and every process it starts every 250 ms (commit, working set, CPU; system CPU and
     available memory) and keeps a handle to each process, so a worker that ended or was killed
     still reports its exact CPU time and final peak memory. It also holds the parsers of Lucent.exe's
     `measure:` lines, the statistics and the batch report check.
   - `Invoke-LucentBench.ps1` runs ten scenarios (start-up, thumbnails, preview, edit, auto-editor,
     auto-cancel, auto-analysis, export, batch, batch-cancel) and writes `machine.json` and one
     `<scenario>.json` per scenario ("lucent.bench-scenario" version 1: the method in words, every
     run with its parsed measurements and resources, and per-group statistics).
     `Invoke-BatchResilience.ps1` runs the 1,000-file gate (decision 6) and writes `resilience.json`
     ("lucent.bench-resilience" version 1). `Format-BenchReport.ps1` renders a baseline's tables from
     that JSON, so a baseline's figures come from its data.
   - The harness only reads the fixtures: the scenarios' folders hold one hard link per fixture
     under the work folder in `build\`, exported images are deleted once checked, and every run
     ends by comparing the fixtures' SHA-256 with their values before it.
   - `tests\bench\Test-LucentBench.ps1` tests the quoting of command lines (round trip through
     CommandLineToArgvW), the statistics, every parser on canned lines, the batch report check and
     the runner and sampler on cmd.exe and ping.exe (CTest `lucent_bench_harness_tests`, label
     `bench`; no build or fixtures needed).

2. **Measurement hooks in Lucent.exe's developer automation** (developer-only options,
   `developer_automation.hpp`; not part of the user interface). With `--measure`, Lucent.exe now
   also reports the first frame its window presented, the first frame of a photo rendered from the
   RAW data after it was added (`--open-first`; the camera's own preview shown meanwhile does not
   count), a batch's timeline (created, first photo done, finished, exported photos per minute),
   the batch scheduler's own statistics (workers started and at most running at once, attempts,
   retries, admissions deferred for memory, the worker budget and aggregate limit, the largest
   worker and the peak commit of all workers together from the group Job Object's accounting;
   `BatchService::measurement_summary()`), the time Auto took to its outcome, and at the end its
   own peak working set, peak commit and CPU time. New options make the latencies measurable:
   `--cancel-batch-after <n>` and `--pause-batch-after <n>` / `--resume-after <ms>` act on a running
   batch as the Exports view does, `--resume-batch` follows the library's unfinished batch after a
   restart (the resilience run's recovery), and `--cancel-auto-after <ms>` cancels Auto as its panel
   does. The new lines are `key=value` fields that the harness parses; the existing lines are
   unchanged.

3. **The method.**
   - *Build and layout.* The `release` preset. Start-up runs use the installed layout that
     `tools\package.ps1 -SkipBuild -StageOnly` stages (Qt deployed by windeployqt, as Setup
     installs it); the other runs use the build's `bin` folder.
   - *Repetitions.* Each scenario starts with one discarded warm-up run, then records three
     repetitions (`-Repetitions`); in the 2026-10-09 baseline auto-cancel, auto-analysis and
     batch-cancel had no warm-up yet (amendment, decision 9). Figures are the median with the minimum and maximum; p95 is the
     nearest rank, which for three repetitions is the maximum, so a p95 over repetitions is only
     quoted where there are many values (images, drag frames). Per-image figures (analysis,
     export) take each image's median over its repetitions first, then the median and p95 over the
     images. The coefficient of variation (sample standard deviation over mean) is recorded for
     every figure and decides what may become a budget (decision 5).
   - *Caches.* The fixtures are read once before the first scenario, so inputs come from the file
     cache unless memory pressure evicted them. "Cold" start is the first start of a fresh copy of
     the installed layout made with unbuffered I/O (`robocopy /J`), so none of Lucent's own files is
     in the file cache, with a new data root: it is a first start after installation, and it
     includes Windows Defender's first look at the new files; Windows' own DLLs and the graphics
     driver stay cached, so it is not a start after a reboot. "Warm" is the same copy and data root
     started again at once. Every other Lucent.exe run gets a new data root (an empty library and
     thumbnail cache) and warm binaries.
   - *Definitions.* Start-up times count from process creation (`GetProcessTimes`); the other
     Lucent.exe times from the automation's start, which is when it asks for the import. Peak
     memory is the operating system's peak counters: commit (`PeakPagefileUsage`, private bytes)
     and working set. "Workers together" is the batch service's Job Object accounting; samples of
     the sum are also kept. CPU use is the CPU time of a program and all its processes over its
     wall time and the 12 logical processors. A cancellation latency is from the request to the
     state the interface shows (Auto: its outcome; a batch: cancelled with nothing running); the
     interface applies batch events at most every 200 ms, so batch figures have that resolution.
   - *The machine.* `machine.json` records the processor, memory, disk, power plan, antivirus,
     memory available and CPU used before the run, and the busiest other processes. A comparison
     between baselines is valid only on a like machine in a like state.

4. **The first baseline** is [`docs/performance/baseline-2026-10-09.md`](../performance/baseline-2026-10-09.md)
   with its data in `docs/performance/data/2026-10-09/`: release build of the W12 part 1 working
   tree, reference machine, 2026-10-09. Medians [min-max] over three repetitions:

   | What | Result |
   |---|---|
   | Warm start: window created / first frame | 0.81 s [0.81-0.83] / 0.95 s [0.94-0.96] |
   | Cold start (first start after installation) | 2.86 s / 3.07 s |
   | First thumbnail: new library / after a restart | 0.18 s / 0.15 s |
   | Photo added to its first RAW frame on screen | 0.80 s (33 MP) to 1.19 s (45 MP); 2.06 s X-Trans (26 MP) |
   | Slider to frame while dragging, median per drag | 90 to 99 ms (exposure), 108 to 126 ms (shadows) |
   | Auto in the editor | 2.05 to 2.17 s kept as shot, 6.36 to 6.39 s with the full check |
   | Automatic analysis per image, 38 images | median 1.61 s, p95 3.01 s |
   | Export per image in process, 37 images | JPEG median 2.58 s, p95 6.30 s; 16-bit TIFF median 8.61 s, p95 23.1 s |
   | Batch of the 40 fixtures through the workers | 22.5 photos per minute (JPEG), 10.4 with Auto |
   | Peak memory | largest worker 1,403 MiB; workers together 1,844 MiB of the 4,559 MiB limit; Lucent.exe 182 MiB idle, about 1.4 GiB with a 45 MP photo at 100 % |
   | Cancellation | Auto 74 ms (analysis) and 245 ms (final check); a batch about 0.2 s (the interface's resolution) |

   The machine was loaded (decision 3; about 2 GB of memory available, other processes busy), so
   these figures are conservative for throughput and memory waits, and part of their variation is
   the machine's drift: the export scenario's third pass ran a median 14 % slower (JPEG) than its
   first two, which agreed within 1 %.

5. **Regression budgets only where the variance allows** (`docs/performance/budgets.json`,
   `tools\bench\Test-BenchBudgets.ps1`). A measured figure becomes a budget only when the baseline
   has at least three values and a coefficient of variation of at most 10 % for times and rates or
   5 % for memory peaks; then a time may not exceed max(1.25 x median, 1.10 x the largest value), a
   memory peak 1.10 x the largest value, and a rate may not fall below min(0.80 x median, 0.90 x the
   smallest value), rounded to three significant digits. The margins are the observed spread plus
   room for the machine's state, not targets. Everything else is tracked: reported beside its
   baseline at each measurement, never failed. Of the 79 candidates, 68 are budgets (start-up warm,
   thumbnails, first RAW preview and 100 % view, drag medians and maxima, Auto per photo and its
   memory, Auto cancelled during the check, analysis per image, export per image and its memory,
   batch throughput and the workers' and Lucent.exe's memory per batch) and 11 are tracked, each
   with its reason in the file: the cold start (antivirus and disk), the warm start's working set
   (6 %), the one-step exposure edit on the Z 6 (one of its three frames was published but never
   recorded as shown), Auto cancelled during the analysis (34 to 80 ms), the five-photo automatic
   batch's workers together (9 %), the four figures of the automatic batch of all fixtures (two runs
   only) and the two batch cancellations (the interface's 200 ms resolution).
   `Test-BenchBudgets.ps1 -DataDir <new run>` compares a new run of the harness on the same machine
   and exits 1 when a budget fails: exceeded, or (amendment, decision 8) missing or not trustworthy.
   An exceeded budget is first measured again
   with the machine idle; if it holds, it is a regression to fix, or a deliberate cost recorded with
   a new baseline and a note in this ADR (new budgets come only from a new baseline,
   `Test-BenchBudgets.ps1 -Create`). No figure here is a promise to users, and the blueprint's
   targets are not turned into budgets.

6. **The 1,000-file batch resilience gate** (`Invoke-BatchResilience.ps1`, blueprint section 10).
   One as-shot JPEG batch of 1,000 distinct files, in a fixed shuffled order: every fixture once,
   100 real copies of five small fixtures, 10 fixtures cut to half their size, and 850 synthetic DNGs
   (0.8 to 2.7 MP; the C# writer follows the tag set of Lucent's test DNGs). Hard links to the
   fixtures cannot make such a corpus: Lucent imports a further hard link of a file it already has
   as a duplicate of that photo (ADR 0008, decision 5), and full copies would need about 31 GB of
   the machine's 13 GB free. During the batch the harness ends a worker twice while it runs a job,
   the automation pauses and resumes it, and the harness ends Lucent.exe at 603 photos;
   `--resume-batch` then continues it after recovery. A separate batch of 100 files is cancelled
   after 20 photos. A watcher hashes every
   output (and removes it after 60 s so 1,000 JPEGs fit the disk) and the report is checked against
   those hashes: one entry per file, each exported file with its receipt's bytes and size, nothing
   unrecorded, no temporary file, failed and skipped entries with reasons, every file of one
   fixture ending the same way, no truncated file exported, originals unchanged, Lucent.exe's commit
   not growing (last tenth of the samples within 64 MiB of the first) and the workers within the
   aggregate limit. Result on 2026-10-09: passed (987 exported and verified, 13 skipped with reasons,
   both crashed jobs retried, the 2 interrupted attempts recovered and redone, Lucent.exe 259 to
   262 MiB of commit). Disk-full and drive removal stay with the batch tests (ADR 0012); a long run
   of large photos is not part of it.

7. **When to measure.** Before each release, and after a change to the engine, the analysis, the
   exporter, the batch scheduler or the workers: run the harness on the reference machine (idle,
   on AC power), write `docs/performance/baseline-<date>.md` from `Format-BenchReport.ps1` when the
   numbers move, and check the budgets. The harness runs no build and changes no source file; it
   needs the fixtures (`tools\fetch-fixtures.ps1 -Tier all`) and about 6 GB of free disk.

## Consequences

- A regression in start-up, previews, editing, Auto, export, batch throughput or memory can now be
  told from noise on the reference machine, and every figure in the baseline can be traced to a
  run, a log and the machine's state. The CTest suite gains `lucent_bench_harness_tests` (157 checks
  of the harness, about 10 s, no build needed).
- The first measurements point at work worth doing, which this ADR does not decide:
  - the exporter's step (colour conversion, encoding, writing, re-reading and verifying) takes most
    of an export: 78 % of the R5 JPEG, 94 % of its 16-bit TIFF;
  - two workers gain little on this machine: the 40-fixture batch (98.6 s) is barely faster than the
    same photos exported one after another in process (102.2 s), with Lucent's processes using half
    the CPU and the second worker deferred for memory 12 to 20 times per batch;
  - Lucent.exe's own memory rises during automatic batches (165 MiB of working set for a JPEG batch
    of five, 887 MiB with Auto), although the analysis runs in the workers;
  - the X-Trans path takes about twice as long as Bayer files of its size for the first preview.
- The budgets come from a loaded 12 GB laptop. They are therefore loose; a baseline on an idle
  machine, and on the blueprint's 16 GB laptop when one is available, should replace them before
  they judge small changes. They are valid for this machine only.
- Lucent.exe carries a few more developer-only options and `measure:` lines, and the UI's
  `BatchService` interface a `measurement_summary()` with a default; neither is visible to users.
  `--resume-batch` makes the restart path testable from the command line.
- The resilience corpus is mostly small synthetic photos: the run proves the batch machinery at
  1,000 jobs (crashes, interruption, recovery, cancellation, reports, flat coordinator memory), not
  sustained work on 1,000 large photos. Hard links of one file being one photo is Lucent's
  intended behaviour, now also documented here.

## Alternatives considered

- **A C++ benchmark program linking Lucent's libraries** (Google Benchmark or a custom driver).
  It would time library calls precisely, but not what the photographer runs: the shell, the worker
  protocol, process starts, the catalog and the interface's own latencies. The fixture tests
  already exercise the libraries; the developer automation already drives the real programs.
- **Sampling with Get-Process alone.** Simpler, but a worker that ends between two samples loses
  its last CPU time and its final peak, and an injected crash loses everything after the previous
  sample; holding a handle to each process keeps both exact.
- **Windows Performance Recorder (ETW) traces.** Precise and detailed, but it needs administrator
  rights, produces gigabytes per run and analysis by hand: a tool for investigating a regression the
  harness finds, not for the baseline.
- **Emptying the standby list for cold starts** (RAMMap, `NtSetSystemInformation`). It needs
  administrator rights and throws away the file cache of the whole machine; a fresh unbuffered copy
  reaches the same state for Lucent's files without either.
- **Performance gates in CI.** There is no dedicated, quiet runner; budgets on shared hardware would
  fail at random and be ignored. The harness runs on the reference machine before a release.
- **Budgets from the blueprint's targets** ("sub-second proxy feedback"). The blueprint calls them
  targets to measure, not demonstrated figures; a budget must come from a measured baseline and its
  variance, and a target the data does not support is not set.

## Amendment: closing review of W12 part 1 (2026-10-09)

The review found that the budget check passed when measurements were missing (an empty `-DataDir`
printed every row "missing" and "0 budgets exceeded.", exit 0) or rested on failed runs, and that
the resilience run passed its memory check with too few samples or without the aggregate limit
(COR-2); and that auto-cancel, auto-analysis and batch-cancel had no discarded warm-up run although
decision 3 says every scenario has one (COR-5).

8. **The checks fail closed.** `Test-BenchBudgets.ps1` exits 2 when `-DataDir` holds no
   measurement, and a budgeted figure fails when it is missing, when its group has more failed runs
   than the baseline's, when it rests on fewer values than the baseline's (repetitions, or the
   images of an over-images statistic), or when an image failed that the baseline exported (the
   baseline's own failures, the Nikon Z 8 HE files and the dual-pixel R5 crop, are not
   regressions); the table names the reason. `Invoke-BatchResilience.ps1` reports a session whose
   memory could not be judged (fewer than 20 samples) and a missing aggregate limit as problems.
   `lucent_bench_harness_tests` checks the gate on the committed baseline (passes) and on altered
   copies (an empty folder, a missing scenario, a failed run, a newly failing image), 165 checks.
9. **Every scenario has its warm-up.** auto-cancel and batch-cancel start with one discarded run of
   their first configuration, auto-analysis with a discarded pass over the smoke tier (one quick run
   of the three scenarios recorded one warm-up each and kept it out of the statistics). The
   2026-10-09 baseline is kept as measured: five budgets (`analysis.median_over_images`,
   `analysis.p95_over_images`, `analysis.whole_run`, `analysis.peak_commit`,
   `auto_cancel.final_check`) include a cold first repetition (the analysis' first pass was 1.5 %
   slower per image than the next two), which makes them at most that much looser. They are derived
   again, with the over-images statistics gated on the repetition variability of the statistic
   itself rather than the median per-image CV, by the next baseline (the idle-machine baseline of
   the W12 licence and release phase).
