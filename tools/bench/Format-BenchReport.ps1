<#
.SYNOPSIS
    Renders the Markdown tables of a performance baseline from the harness's JSON
    (docs\performance\data\<date>), so a baseline's figures come straight from its data.

.DESCRIPTION
    Reads machine.json, the scenario files written by Invoke-LucentBench.ps1 and resilience.json
    (Invoke-BatchResilience.ps1) when present, and writes one Markdown section per file: each figure
    as "median [min-max]" over the recorded repetitions with the coefficient of variation, or as a
    distribution over images. Missing files are skipped.

.PARAMETER DataDir
    The folder with the JSON files.
.PARAMETER OutFile
    Where to write the Markdown (default: standard output).

.EXAMPLE
    powershell -NoProfile -File tools\bench\Format-BenchReport.ps1 -DataDir docs\performance\data\2026-10-10

.NOTES
    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataDir,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Import-Module (Join-Path $PSScriptRoot 'LucentBench.psm1') -Force
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$lines = New-Object System.Collections.ArrayList

function Out-Line { param([string]$Text = '') [void]$lines.Add($Text) }

function Format-Number {
    param($Value, [int]$Digits = 0)
    if ($null -eq $Value -or $Value -is [string]) { return '-' }
    return ([double]$Value).ToString("N$Digits", $inv)
}

function Format-Percent {
    # A fraction (0.05) as a percentage ("5"); "-" when unknown.
    param($Fraction)
    if ($null -eq $Fraction -or $Fraction -is [string]) { return '-' }
    return Format-Number (100.0 * [double]$Fraction) 0
}

function Format-Stat {
    # "median [min-max]" with the coefficient of variation; scale converts units (0.001: ms to s).
    param($Stat, [int]$Digits = 0, [double]$Scale = 1.0, [switch]$NoCv)
    if ($null -eq $Stat -or $null -eq (Get-BenchValue $Stat 'n') -or [int](Get-BenchValue $Stat 'n') -eq 0) { return '-' }
    $median = [double]$Stat.median * $Scale
    $text = Format-Number $median $Digits
    if ([int]$Stat.n -gt 1) {
        $text += ' [' + (Format-Number ([double]$Stat.min * $Scale) $Digits) + '-' + (Format-Number ([double]$Stat.max * $Scale) $Digits) + ']'
        if (-not $NoCv -and $null -ne $Stat.cv) { $text += ' (' + (Format-Percent $Stat.cv) + ' %)' }
    }
    return $text
}

function Get-Group {
    param($Document, [string]$Name)
    return Get-BenchValue $Document.summary $Name
}

function Read-Scenario {
    param([string]$Name)
    $path = Join-Path $DataDir "$Name.json"
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return Read-BenchJson -Path $path
}

# --- Machine ---------------------------------------------------------------------------------------
$machinePath = Join-Path $DataDir 'machine.json'
if (Test-Path -LiteralPath $machinePath) {
    $m = Read-BenchJson -Path $machinePath
    Out-Line '### Machine and build'
    Out-Line
    Out-Line '| Fact | Value |'
    Out-Line '|---|---|'
    Out-Line "| Processor | $($m.cpu.name), $($m.cpu.cores) cores / $($m.cpu.logical_processors) threads |"
    Out-Line "| Memory | $($m.memory.total_gib) GiB; $($m.memory.available_gib_at_start) GiB available at the start |"
    Out-Line "| System | $($m.os.caption) $($m.os.version) |"
    if ($m.PSObject.Properties['system_disk']) { Out-Line "| System disk | $($m.system_disk.model), $($m.system_disk.bus) $($m.system_disk.media) |" }
    if ($m.PSObject.Properties['gpu']) { Out-Line "| GPU | $(@($m.gpu) -join ', ') |" }
    Out-Line "| Power | $($m.power_scheme); on AC power: $($m.on_ac_power) |"
    if ($m.PSObject.Properties['antivirus'] -and $m.antivirus -isnot [string]) {
        Out-Line "| Antivirus | $($m.antivirus.product), real-time protection $($m.antivirus.real_time_protection) |"
    }
    $busiest = '-'
    if ($m.PSObject.Properties['busiest_other_processes_percent']) {
        $busiest = (@($m.busiest_other_processes_percent) | ForEach-Object { "$_ %" }) -join ', '
    }
    Out-Line "| Background CPU before the run | $(Format-Number $m.idle_cpu_percent_before 1) % of the machine; the busiest other processes $busiest of one processor |"
    Out-Line "| Build | $($m.build.CMAKE_BUILD_TYPE), $($m.build.VCPKG_TARGET_TRIPLET); commit $($m.git.commit) on $($m.git.branch), $($m.git.uncommitted_paths) uncommitted paths |"
    Out-Line "| Harness | $($m.harness.repetitions) repetitions, sampling every $($m.harness.sample_ms) ms, PowerShell $($m.harness.powershell); app layout $($m.harness.app_dir) |"
    if ($m.PSObject.Properties['duration_minutes']) { Out-Line "| Duration | $(Format-Number $m.duration_minutes 0) min; fixtures unchanged: $(@($m.sources_changed).Count -eq 0) |" }
    Out-Line
}

# --- Start-up and thumbnails ---------------------------------------------------------------------------
$d = Read-Scenario 'startup'
if ($d) {
    Out-Line '### Start-up'
    Out-Line
    Out-Line '| Start | Window created (s) | First frame (s) | Peak working set (MiB) | CPU (s) |'
    Out-Line '|---|---|---|---|---|'
    foreach ($name in @('cold', 'warm')) {
        $g = Get-Group $d $name
        if (-not $g) { continue }
        Out-Line ("| {0} | {1} | {2} | {3} | {4} |" -f $name, (Format-Stat $g.'metrics.window_loaded_ms' 2 0.001),
            (Format-Stat $g.'metrics.first_frame_ms' 2 0.001), (Format-Stat $g.'metrics.process.peak_working_set_mib' 0),
            (Format-Stat $g.'metrics.process.cpu_s' 2))
    }
    Out-Line
}
$d = Read-Scenario 'thumbnails'
if ($d) {
    Out-Line '### Thumbnails'
    Out-Line
    Out-Line '| Library | First thumbnail (s) | Window created (s) | Peak working set (MiB) |'
    Out-Line '|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} |" -f $p.Name, (Format-Stat $g.'metrics.first_thumbnail_ms' 2 0.001),
            (Format-Stat $g.'metrics.window_loaded_ms' 2 0.001), (Format-Stat $g.'metrics.process.peak_working_set_mib' 0))
    }
    Out-Line
}

# --- Preview, edit, Auto ----------------------------------------------------------------------------
$d = Read-Scenario 'preview'
if ($d) {
    Out-Line '### First RAW preview'
    Out-Line
    Out-Line '| Photo | MP | Added to first RAW frame (s) | Opened to first RAW frame (s) | Full-size render (s) | 100 % view visible (s) | Peak working set (MiB) |'
    Out-Line '|---|---|---|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} |" -f $p.Name, (Format-Number (Get-BenchValue $d.extra.megapixels $p.Name) 1),
            (Format-Stat $g.'metrics.first_raw_preview_ms' 2 0.001),
            (Format-Stat $g.'metrics.fit_preview.visible_ms' 2 0.001), (Format-Stat $g.'metrics.fit_preview.engine_s' 2 1 -NoCv),
            (Format-Stat $g.'metrics.zoom_preview.visible_ms' 2 0.001), (Format-Stat $g.'metrics.process.peak_working_set_mib' 0))
    }
    Out-Line
}
$d = Read-Scenario 'edit'
if ($d) {
    Out-Line '### Edit latency (Fit view)'
    Out-Line
    Out-Line '| Photo and drag | Exposure step (ms) | Contrast step (ms) | Drag: median (ms) | Drag: 90th pct (ms) | Drag: max (ms) | Develop service median (ms) | Quick frames | Final frame after release (ms) | Largest UI gap (ms) |'
    Out-Line '|---|---|---|---|---|---|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} |" -f $p.Name,
            (Format-Stat $g.'metrics.edit_exposure=1' 0), (Format-Stat $g.'metrics.edit_contrast=20' 0),
            (Format-Stat $g.'metrics.drag.shown_median_ms' 0), (Format-Stat $g.'metrics.drag.shown_p90_ms' 0),
            (Format-Stat $g.'metrics.drag.shown_max_ms' 0), (Format-Stat $g.'metrics.drag.service_median_ms' 0),
            (Format-Stat $g.'metrics.drag.quick' 0 1 -NoCv), (Format-Stat $g.'metrics.drag.final_after_release_ms' 0),
            (Format-Stat $g.'metrics.drag.ui_gap_max_ms' 0 1 -NoCv))
    }
    Out-Line
}
$d = Read-Scenario 'auto-editor'
if ($d) {
    Out-Line '### Auto in the editor'
    Out-Line
    Out-Line '| Photo | MP | Request to outcome (s) | Lucent.exe peak commit (MiB) | CPU (s) |'
    Out-Line '|---|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} | {4} |" -f $p.Name, (Format-Number (Get-BenchValue $d.extra.megapixels $p.Name) 1),
            (Format-Stat $g.'metrics.auto.ended_after_ms' 2 0.001), (Format-Stat $g.'metrics.process.peak_commit_mib' 0),
            (Format-Stat $g.'metrics.process.cpu_s' 1))
    }
    Out-Line
}
$d = Read-Scenario 'auto-cancel'
if ($d) {
    Out-Line '### Auto cancellation'
    Out-Line
    Out-Line '| Cancelled | Run progress at the request | Request to the run ending (ms) | Ended cancelled |'
    Out-Line '|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} |" -f $p.Name, (Format-Stat $g.'metrics.auto_cancel.progress' 2 1 -NoCv),
            (Format-Stat $g.'metrics.auto_cancel.ended_after_request_ms' 0), (Format-Stat $g.'metrics.auto.cancelled' 0 1 -NoCv))
    }
    Out-Line
}
$d = Read-Scenario 'auto-analysis'
if ($d) {
    $o = $d.extra.over_images
    Out-Line '### Automatic analysis per image (lucent-auto)'
    Out-Line
    $cv = Format-Percent (Get-BenchValue $o 'repetition_cv.median')
    Out-Line (("Over {0} images (each image's median over the repetitions): analysis {1} s median, p95 {2} s, " +
            "{3} s per megapixel median (p95 {4}); repetition CV median {5} %. Failed: {6}.") -f $o.analysis_seconds.n,
        (Format-Number $o.analysis_seconds.median 2), (Format-Number $o.analysis_seconds.p95 2),
        (Format-Number $o.seconds_per_megapixel.median 3), (Format-Number $o.seconds_per_megapixel.p95 3),
        $cv, ((@($d.extra.failed_images) -join ', ')))
    Out-Line
    Out-Line '| Image | MP | Analysis (s) | s/MP |'
    Out-Line '|---|---|---|---|'
    foreach ($p in $d.extra.per_image.PSObject.Properties) {
        Out-Line ("| {0} | {1} | {2} | {3} |" -f $p.Name, (Format-Number $p.Value.megapixels 1),
            (Format-Stat $p.Value.analysis_seconds 2), (Format-Number $p.Value.seconds_per_megapixel.median 3))
    }
    Out-Line
    $whole = Get-Group $d 'lucent-auto'
    Out-Line ("Whole run: {0} s; lucent-auto peak commit {1} MiB; CPU {2} % of the machine." -f (Format-Stat $whole.seconds 0),
        (Format-Stat $whole.'resources.root.peak_commit_mib' 0), (Format-Stat $whole.'resources.cpu_percent_of_machine' 0))
    Out-Line
}

# --- Export -----------------------------------------------------------------------------------------
$d = Read-Scenario 'export'
if ($d) {
    Out-Line '### Export per image (lucent-render, in process)'
    Out-Line
    Out-Line '| Format | Images | Seconds: median | p95 | s/MP: median | p95 | Repetition CV (median) | Peak commit (MiB): median | max | Failed |'
    Out-Line '|---|---|---|---|---|---|---|---|---|---|'
    foreach ($p in $d.extra.over_images.PSObject.Properties) {
        $o = $p.Value
        Out-Line ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} % | {7} | {8} | {9} |" -f $p.Name, $o.images,
            (Format-Number $o.seconds.median 2), (Format-Number $o.seconds.p95 2), (Format-Number $o.seconds_per_megapixel.median 3),
            (Format-Number $o.seconds_per_megapixel.p95 3), (Format-Percent (Get-BenchValue $o 'repetition_cv.median')),
            (Format-Number $o.peak_commit_mib.median 0), (Format-Number $o.peak_commit_mib.max 0), (@($o.failed) -join ', '))
    }
    Out-Line
    Out-Line '| Image | JPEG (s) | JPEG render / export (s) | TIFF16 (s) | TIFF16 export (s) | JPEG peak commit (MiB) |'
    Out-Line '|---|---|---|---|---|---|'
    $ids = @($d.runs | Where-Object { -not $_.warmup } | ForEach-Object { $_.fixture } | Sort-Object -Unique)
    foreach ($id in $ids) {
        $jpeg = Get-Group $d "jpeg/$id"
        $tiff = Get-Group $d "tiff16/$id"
        if (-not $jpeg -and -not $tiff) { continue }
        if ([int](Get-BenchValue $jpeg 'seconds.n') -eq 0 -and [int](Get-BenchValue $tiff 'seconds.n') -eq 0) {
            $why = @($d.runs | Where-Object { $_.fixture -eq $id -and $_.error } | Select-Object -First 1 | ForEach-Object { $_.error })
            Out-Line ("| {0} | failed: {1} | | | | |" -f $id, ($why -join ''))
            continue
        }
        $render = $null
        if ($jpeg) {
            $one = Get-BenchValue $jpeg 'render_stage_one_s.median'
            $two = Get-BenchValue $jpeg 'render_stage_two_s.median'
            if ($null -ne $one -and $null -ne $two) { $render = [double]$one + [double]$two }
        }
        Out-Line ("| {0} | {1} | {2} / {3} | {4} | {5} | {6} |" -f $id, (Format-Stat (Get-BenchValue $jpeg 'seconds') 2),
            (Format-Number $render 2), (Format-Number (Get-BenchValue $jpeg 'export_s.median') 2), (Format-Stat (Get-BenchValue $tiff 'seconds') 2),
            (Format-Number (Get-BenchValue $tiff 'export_s.median') 2), (Format-Number (Get-BenchValue $jpeg 'peak_commit_mib.median') 0))
    }
    Out-Line
}

# --- Batches -----------------------------------------------------------------------------------------
$d = Read-Scenario 'batch'
if ($d) {
    Out-Line '### Batches through the workers'
    Out-Line
    Out-Line '| Batch | Exported | Submission (s) | Batch (s) | Photos per minute | s per photo | Workers together, peak commit (MiB) | Largest worker (MiB) | Lucent.exe peak working set (MiB) | CPU, % of machine | Report check |'
    Out-Line '|---|---|---|---|---|---|---|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        $problems = @($d.runs | Where-Object { $_.name -eq $p.Name -and -not $_.warmup } |
                ForEach-Object { @(Get-BenchValue $_ 'report_check.problems') } | Where-Object { $_ })
        $check = 'consistent'
        if ($problems.Count -gt 0) { $check = "$($problems.Count) problems" }
        Out-Line ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |" -f $p.Name,
            (Format-Stat $g.'metrics.batch_timeline.exported' 0 1 -NoCv), (Format-Stat $g.'derived.submission_s' 1),
            (Format-Stat $g.'metrics.batch_timeline.seconds' 1), (Format-Stat $g.'metrics.batch_timeline.per_minute' 1),
            (Format-Stat $g.'derived.seconds_per_exported' 2 1 -NoCv), (Format-Stat $g.'metrics.batch_scheduler.peak_aggregate_commit_mib' 0),
            (Format-Stat $g.'metrics.batch_scheduler.peak_worker_commit_mib' 0), (Format-Stat $g.'metrics.process.peak_working_set_mib' 0),
            (Format-Stat $g.'resources.cpu_percent_of_machine' 0), $check)
    }
    Out-Line
}
$d = Read-Scenario 'batch-cancel'
if ($d) {
    Out-Line '### Batch cancellation'
    Out-Line
    Out-Line '| Batch | Photos running at the request | Request to stopped (ms) | Exported | Cancelled | Report check |'
    Out-Line '|---|---|---|---|---|---|'
    foreach ($p in $d.summary.PSObject.Properties) {
        $g = $p.Value
        $problems = @($d.runs | Where-Object { $_.name -eq $p.Name } | ForEach-Object { @(Get-BenchValue $_ 'report_check.problems') } |
                Where-Object { $_ })
        $check = 'consistent'
        if ($problems.Count -gt 0) { $check = "$($problems.Count) problems" }
        Out-Line ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f $p.Name, (Format-Stat $g.'metrics.batch_cancel.running' 0 1 -NoCv),
            (Format-Stat $g.'metrics.batch_cancel.stopped_after_ms' 0), (Format-Stat $g.'metrics.batch_timeline.exported' 0 1 -NoCv),
            (Format-Stat $g.'metrics.batch_timeline.cancelled' 0 1 -NoCv), $check)
    }
    Out-Line
}

# --- Resilience --------------------------------------------------------------------------------------
$path = Join-Path $DataDir 'resilience.json'
if (Test-Path -LiteralPath $path) {
    $r = Read-BenchJson -Path $path
    Out-Line '### 1,000-file batch resilience run'
    Out-Line
    Out-Line ("Checks passed: {0}. Problems: {1}." -f $r.checks.passed, $(if (@($r.checks.problems).Count -eq 0) { 'none' } else { @($r.checks.problems) -join '; ' }))
    Out-Line
    Out-Line '| Item | Result |'
    Out-Line '|---|---|'
    Out-Line "| Corpus | $($r.corpus.files) files: $($r.corpus.fixtures) fixtures, $($r.corpus.fixture_copies) copies of small fixtures, $($r.corpus.truncated_copies) truncated copies, $($r.corpus.synthetic_dngs) synthetic DNGs ($(@($r.corpus.synthetic_sizes) -join ', ')) |"
    $outcomes = @($r.checks.main_report.outcomes.PSObject.Properties | ForEach-Object { "$($_.Value) $($_.Name)" }) -join ', '
    Out-Line "| Main batch report | $($r.checks.main_report.entries) entries: $outcomes; $($r.checks.main_report.outputs_hashed) outputs hashed by the watcher; $($r.checks.main_report.entries_with_retries) entries with more than one attempt |"
    foreach ($kind in $r.checks.main_report.outcomes_by_kind.PSObject.Properties) {
        $text = @($kind.Value.PSObject.Properties | ForEach-Object { "$($_.Value) $($_.Name)" }) -join ', '
        Out-Line "| Outcomes, $($kind.Name) files | $text |"
    }
    $cancelled = @($r.checks.cancelled_report.outcomes.PSObject.Properties | ForEach-Object { "$($_.Value) $($_.Name)" }) -join ', '
    Out-Line "| Cancelled batch | $($r.checks.cancelled_report.entries) entries: $cancelled |"
    foreach ($i in @($r.injections)) {
        Out-Line ("| Injected: {0} | at {1} photos done, {2:N0} s into the session |" -f $i.kind, $i.at_done, [double]$i.seconds)
    }
    foreach ($key in @('session1', 'session2')) {
        $mem = Get-BenchValue $r.checks.memory $key
        if ($mem -and $null -ne (Get-BenchValue $mem 'lucent_commit_growth_mib')) {
            Out-Line ("| Memory, {0} | Lucent.exe commit {1} MiB (first tenth) to {2} MiB (last tenth), trend {3} MiB/h, max {4} MiB; workers together max {5} MiB of {6} MiB; bounded: {7} |" -f $key,
                (Format-Number $mem.lucent_commit_first_tenth_median_mib 0), (Format-Number $mem.lucent_commit_last_tenth_median_mib 0),
                (Format-Number $mem.lucent_commit_trend_mib_per_hour 0), (Format-Number $mem.lucent_commit_max_mib 0),
                (Format-Number $mem.workers_commit_mib.max 0), (Format-Number $mem.aggregate_limit_mib 0), $mem.bounded)
        }
    }
    foreach ($key in @('session1', 'session2')) {
        $t = Get-BenchValue $r.throughput $key
        if ($t) { Out-Line ("| Throughput, {0} | {1} photos per minute ({2} to {3} done in {4} min) |" -f $key, (Format-Number $t.per_minute 1), $t.from_done, $t.to_done, (Format-Number $t.minutes 1)) }
    }
    Out-Line "| Duration | $(Format-Number $r.duration_minutes 0) min |"
    Out-Line
}

$text = ($lines -join "`n") + "`n"
if ($OutFile) { [System.IO.File]::WriteAllText($OutFile, $text, (New-Object System.Text.UTF8Encoding($false))) } else { $text }
