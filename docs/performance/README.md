# Performance measurements

This folder holds Lucent's measured performance: the baselines (`baseline-<date>.md`), the raw data
behind them (`data/<date>/*.json`), the regression budgets derived from them (`budgets.json`) and
how to measure again. The method and the regression budgets are decided in
[ADR 0021](../adr/0021-performance-baseline.md). Blueprint section 10 ("Performance targets to
measure") lists what to measure; its targets are engineering targets, not demonstrated figures, and
nothing here turns them into claims the data does not support.

| Baseline | Build | Machine | Notes |
|---|---|---|---|
| [2026-10-09](baseline-2026-10-09.md) | release preset, branch `feat/w12-hardening` (W12 part 1 working tree on e51dd65) | Ryzen 5 5500U, 11.9 GB, Windows 11 | the first baseline and the 1,000-file resilience gate (passed); the machine was a loaded development laptop (see the baseline) |

## The harness

Everything runs the shipped programs, each run in its own process, from a release build:

```powershell
powershell -NoProfile -File tools\build.ps1 -Preset release -BuildDir build\w12-perf -Jobs 6
powershell -NoProfile -File tools\package.ps1 -BuildDir build\w12-perf -SkipBuild -StageOnly   # installed layout for start-up runs
powershell -NoProfile -File tools\bench\Invoke-LucentBench.ps1 -BuildDir build\w12-perf          # about 1 h
powershell -NoProfile -File tools\bench\Invoke-BatchResilience.ps1 -BuildDir build\w12-perf      # about 10 min, 4 GB of disk
powershell -NoProfile -File tools\bench\Format-BenchReport.ps1 -DataDir docs\performance\data\<date>  # the tables
powershell -NoProfile -File tools\bench\Test-BenchBudgets.ps1 -DataDir docs\performance\data\<date>   # against budgets.json
```

| File | What it is |
|---|---|
| `tools\bench\LucentBench.psm1` | Shared functions. `BenchProcess` (C#, compiled by Add-Type) starts a program with its output captured line by line, samples it and every process it starts (commit, working set, CPU) and keeps a handle to each so an ended worker still reports its exact CPU time. Parsers for Lucent.exe's `measure:` lines, statistics, the batch report check, machine facts. |
| `tools\bench\Invoke-LucentBench.ps1` | The scenarios below; writes `machine.json` and one `<scenario>.json` per scenario. `-Scenario` picks some, `-Repetitions` (default 3), `-Quick` checks the harness itself. |
| `tools\bench\Invoke-BatchResilience.ps1` | The 1,000-file batch resilience run; writes `resilience.json`. |
| `tools\bench\Format-BenchReport.ps1` | Renders a baseline's Markdown tables from its JSON. |
| `tools\bench\Test-BenchBudgets.ps1` | `-Create` derives `budgets.json` from a baseline; otherwise checks a new run against it (exit code 1 when a budget is exceeded). |
| `tests\bench\Test-LucentBench.ps1` | The harness's tests (CTest `lucent_bench_harness_tests`, label `bench`). |

The harness only reads the fixtures: the scenarios' folders hold one hard link per fixture under the
work folder (`build\...`), every Lucent.exe run gets its own `--data-root`, and exported images are
deleted once they have been checked. It ends with the fixtures' SHA-256 compared with their values
before the run. (Lucent imports a second hard link of a file it already has as a duplicate of that
photo, so the 1,000-file run uses real copies and synthetic DNGs instead: see its script.)

### Scenarios

| Scenario | Program and options | Measured |
|---|---|---|
| `startup` | `Lucent.exe --measure` | window created and first frame presented after process creation; cold (fresh unbuffered copy of the installed layout, new data root) and warm (the same again) |
| `thumbnails` | `Lucent.exe --import <folder> --measure` | first thumbnail visible after the import request: new library (camera previews) and after a restart (disk cache) |
| `preview` | `Lucent.exe --import <photo> --open-first --zoom 100 --measure` | first RAW preview (Fit) and the 100 % view |
| `edit` | `--set exposure=1 --set contrast=20 --drag exposure`, `--drag shadows` | one-step edit to its frame on screen, slider position to frame (60 Hz synthetic drag), UI-thread gaps |
| `auto-editor` | `Lucent.exe --open-first --auto --measure` | Auto in the editor, request to outcome (analysis and full-resolution check), process peak memory |
| `auto-cancel` | `--auto --cancel-auto-after 1000` / `4000` | cancel request to the run's end |
| `auto-analysis` | `lucent-auto --group-by none --tier all` | the analysis alone per image |
| `export` | `lucent-render --format jpeg` / `tiff16 --report` | per-image export in one process: decode, render, encode, write, verify |
| `batch` | `Lucent.exe --export-batch <folder> --preset jpeg/tiff [--automatic auto] --measure --quit-when-done` | photos per minute, submission time, the scheduler's figures (workers, peak commit of all workers together), Lucent.exe and worker memory, CPU use; every report checked against the files |
| `batch-cancel` | the same with `--cancel-batch-after <n>` | cancel request to the batch stopped |

Lucent.exe's measurement hooks (`--measure` lines, `--cancel-batch-after`, `--pause-batch-after`,
`--resume-after`, `--resume-batch`, `--cancel-auto-after`) are described in
`apps\lucent\src\developer_automation.hpp`.

## Reading the numbers

- **Median and p95.** Each figure is the median over the recorded repetitions; p95 is the nearest
  rank, so with three repetitions it is the largest. Distributions over images (export, analysis)
  take each image's median first.
- **Warm-up and caches.** Each scenario starts with one discarded run. The fixtures are read once
  before the first scenario, so inputs come from the file cache unless memory pressure evicted
  them. Only the cold start runs read Lucent's own files from disk; Windows' own DLLs and the GPU
  driver stay cached, so "cold" is a first start after installation, not after a reboot.
- **The machine.** `machine.json` records the processor, memory, disk, power plan, antivirus, the
  available memory and CPU use before the run and the busiest other processes. Compare baselines
  only from like machines and states.
- **What is not measured** is listed in each baseline.

## Regression budgets

`budgets.json` holds 79 measurements of the 2026-10-09 baseline. 68 are budgets: a time may not
exceed max(1.25 x median, 1.10 x the largest value), a memory peak 1.10 x the largest value, a rate
may not fall below min(0.80 x median, 0.90 x the smallest value). Only measurements with at least
three values and a small variation (CV at most 10 %, memory 5 %) are budgets; the other 11 are
tracked with their reason (ADR 0021, decision 5). They hold for the reference machine only, and the
2026-10-09 baseline was measured on a loaded machine: measure an exceeded budget again with the
machine idle before treating it as a regression, and replace the budgets from a quieter baseline
when one exists.
