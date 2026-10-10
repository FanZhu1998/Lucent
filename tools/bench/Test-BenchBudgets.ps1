<#
.SYNOPSIS
    Derives Lucent's performance regression budgets from a baseline, or checks a new measurement
    against them (docs\performance\budgets.json; ADR 0021, decision 5).

.DESCRIPTION
    -Create: evaluates the candidate measurements below on a baseline's JSON (Invoke-LucentBench.ps1)
    and writes budgets.json ("lucent.bench-budgets" version 1). Each candidate becomes a budget only
    when its variance allows (New-BenchBudget: at least three values; CV at most 10 % for times and
    rates, 5 % for memory peaks); otherwise it is tracked, with the reason.

    Without -Create: reads -DataDir (a new run of the harness on the same machine), compares every
    candidate with its budget and prints a Markdown table; exit code 1 when a budget fails, 2 when
    -DataDir holds no measurement at all, 0 otherwise. A budget fails when its measurement is above
    its maximum (times, memory) or below its minimum (rates), and also when it cannot be trusted
    (COR-2, W12): missing (a scenario that did not run or crashed, a renamed metric, a wrong
    -DataDir), measured by more failed runs than the baseline's, over fewer values than the
    baseline's (repetitions, or images of an over-images statistic), or over images of which one
    failed that the baseline exported (a regression that makes the slowest photos fail would
    otherwise lower the statistic). A tracked measurement is shown beside its baseline and never
    fails the check. Compare only runs from the same machine in a like state (machine.json).

.PARAMETER Create
    Write budgets from -BaselineDir.
.PARAMETER BaselineDir
    The baseline's data folder (default docs\performance\data\2026-10-09).
.PARAMETER DataDir
    The new run's data folder (check mode).
.PARAMETER Budgets
    budgets.json (default docs\performance\budgets.json).

.EXAMPLE
    powershell -NoProfile -File tools\bench\Test-BenchBudgets.ps1 -Create
.EXAMPLE
    powershell -NoProfile -File tools\bench\Test-BenchBudgets.ps1 -DataDir docs\performance\data\2026-11-02

.NOTES
    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [switch]$Create,
    [string]$BaselineDir = 'docs\performance\data\2026-10-09',
    [string]$DataDir,
    [string]$Budgets = 'docs\performance\budgets.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $PSScriptRoot 'LucentBench.psm1') -Force

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repoRoot $Path
}

# --- Candidates --------------------------------------------------------------------------------------
# group: a group of a scenario's summary with its metric; stat: a statistics object in the file (its
# value and the path of its repetition CV). tracked: a reason that keeps it from being a budget.

$smoke = [ordered]@{ r5 = 'canon-eos-r5-cr3-raw'; a7iv = 'sony-ilce-7m4-arw-compressed'; dji = 'dji-fc4382-dng'
    xt4 = 'fujifilm-x-t4-raf-lossless'; z6 = 'nikon-z-6-nef-lossless' }
$candidates = New-Object System.Collections.ArrayList
function Add-Candidate { param([hashtable]$C) [void]$candidates.Add($C) }

Add-Candidate @{ id = 'startup.cold.first_frame'; file = 'startup'; group = 'cold'; metric = 'metrics.first_frame_ms'; kind = 'time'; unit = 'ms'
    tracked = 'a cold start includes the antivirus scan of new files and the disk: it measures the machine as much as Lucent' }
Add-Candidate @{ id = 'startup.warm.window_created'; file = 'startup'; group = 'warm'; metric = 'metrics.window_loaded_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'startup.warm.first_frame'; file = 'startup'; group = 'warm'; metric = 'metrics.first_frame_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'startup.warm.peak_working_set'; file = 'startup'; group = 'warm'; metric = 'metrics.process.peak_working_set_mib'; kind = 'memory'; unit = 'MiB' }
Add-Candidate @{ id = 'thumbnails.new_library.first'; file = 'thumbnails'; group = 'smoke-new-library'; metric = 'metrics.first_thumbnail_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'thumbnails.restart.first'; file = 'thumbnails'; group = 'smoke-restart'; metric = 'metrics.first_thumbnail_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'thumbnails.all_new_library.peak_working_set'; file = 'thumbnails'; group = 'all-new-library'; metric = 'metrics.process.peak_working_set_mib'; kind = 'memory'; unit = 'MiB' }
foreach ($key in $smoke.Keys) {
    $id = $smoke[$key]
    Add-Candidate @{ id = "preview.$key.first_raw_frame"; file = 'preview'; group = $id; metric = 'metrics.first_raw_preview_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "preview.$key.view_100"; file = 'preview'; group = $id; metric = 'metrics.zoom_preview.visible_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "preview.$key.peak_working_set"; file = 'preview'; group = $id; metric = 'metrics.process.peak_working_set_mib'; kind = 'memory'; unit = 'MiB' }
    Add-Candidate @{ id = "auto_editor.$key.run"; file = 'auto-editor'; group = $id; metric = 'metrics.auto.ended_after_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "auto_editor.$key.peak_commit"; file = 'auto-editor'; group = $id; metric = 'metrics.process.peak_commit_mib'; kind = 'memory'; unit = 'MiB' }
}
foreach ($photo in @('r5', 'z6')) {
    $id = $smoke[$photo]
    Add-Candidate @{ id = "edit.$photo.exposure_step"; file = 'edit'; group = "$id-exposure"; metric = 'metrics.edit_exposure=1'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "edit.$photo.exposure_drag_median"; file = 'edit'; group = "$id-exposure"; metric = 'metrics.drag.shown_median_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "edit.$photo.exposure_drag_max"; file = 'edit'; group = "$id-exposure"; metric = 'metrics.drag.shown_max_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "edit.$photo.exposure_service_median"; file = 'edit'; group = "$id-exposure"; metric = 'metrics.drag.service_median_ms'; kind = 'time'; unit = 'ms' }
    Add-Candidate @{ id = "edit.$photo.shadows_drag_median"; file = 'edit'; group = "$id-shadows"; metric = 'metrics.drag.shown_median_ms'; kind = 'time'; unit = 'ms' }
}
Add-Candidate @{ id = 'auto_cancel.analysis'; file = 'auto-cancel'; group = "$($smoke.r5)-after-1000-ms"; metric = 'metrics.auto_cancel.ended_after_request_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'auto_cancel.final_check'; file = 'auto-cancel'; group = "$($smoke.r5)-after-4000-ms"; metric = 'metrics.auto_cancel.ended_after_request_ms'; kind = 'time'; unit = 'ms' }
Add-Candidate @{ id = 'analysis.median_over_images'; file = 'auto-analysis'; stat = 'extra.over_images.analysis_seconds'; value = 'median'; cv = 'extra.over_images.repetition_cv.median'; failed = 'extra.failed_images'; kind = 'time'; unit = 's' }
Add-Candidate @{ id = 'analysis.p95_over_images'; file = 'auto-analysis'; stat = 'extra.over_images.analysis_seconds'; value = 'p95'; cv = 'extra.over_images.repetition_cv.median'; failed = 'extra.failed_images'; kind = 'time'; unit = 's' }
Add-Candidate @{ id = 'analysis.whole_run'; file = 'auto-analysis'; group = 'lucent-auto'; metric = 'seconds'; kind = 'time'; unit = 's' }
Add-Candidate @{ id = 'analysis.peak_commit'; file = 'auto-analysis'; group = 'lucent-auto'; metric = 'resources.root.peak_commit_mib'; kind = 'memory'; unit = 'MiB' }
foreach ($format in @('jpeg', 'tiff16')) {
    Add-Candidate @{ id = "export.$format.median_over_images"; file = 'export'; stat = "extra.over_images.$format.seconds"; value = 'median'; cv = "extra.over_images.$format.repetition_cv.median"; failed = "extra.over_images.$format.failed"; kind = 'time'; unit = 's' }
    Add-Candidate @{ id = "export.$format.p95_over_images"; file = 'export'; stat = "extra.over_images.$format.seconds"; value = 'p95'; cv = "extra.over_images.$format.repetition_cv.median"; failed = "extra.over_images.$format.failed"; kind = 'time'; unit = 's' }
    Add-Candidate @{ id = "export.$format.r5"; file = 'export'; group = "$format/$($smoke.r5)"; metric = 'seconds'; kind = 'time'; unit = 's' }
    Add-Candidate @{ id = "export.$format.r5.peak_commit"; file = 'export'; group = "$format/$($smoke.r5)"; metric = 'peak_commit_mib'; kind = 'memory'; unit = 'MiB' }
}
Add-Candidate @{ id = 'export.jpeg.a7rv.peak_commit'; file = 'export'; group = 'jpeg/sony-ilce-7rm5-arw-compressed'; metric = 'peak_commit_mib'; kind = 'memory'; unit = 'MiB' }
foreach ($batch in @('smoke-jpeg', 'smoke-tiff16', 'smoke-auto-jpeg', 'all-jpeg', 'all-auto-jpeg')) {
    Add-Candidate @{ id = "batch.$batch.per_minute"; file = 'batch'; group = $batch; metric = 'metrics.batch_timeline.per_minute'; kind = 'rate'; unit = 'photos/min' }
    Add-Candidate @{ id = "batch.$batch.largest_worker"; file = 'batch'; group = $batch; metric = 'metrics.batch_scheduler.peak_worker_commit_mib'; kind = 'memory'; unit = 'MiB' }
    Add-Candidate @{ id = "batch.$batch.workers_together"; file = 'batch'; group = $batch; metric = 'metrics.batch_scheduler.peak_aggregate_commit_mib'; kind = 'memory'; unit = 'MiB' }
    Add-Candidate @{ id = "batch.$batch.lucent_peak_working_set"; file = 'batch'; group = $batch; metric = 'metrics.process.peak_working_set_mib'; kind = 'memory'; unit = 'MiB' }
}
foreach ($cancel in @('all-jpeg-cancel-after-2', 'smoke-tiff16-cancel-after-1')) {
    Add-Candidate @{ id = "batch_cancel.$cancel"; file = 'batch-cancel'; group = $cancel; metric = 'metrics.batch_cancel.stopped_after_ms'; kind = 'time'; unit = 'ms'
        tracked = 'the interface applies batch events at most every 200 ms, the size of the whole measurement' }
}

# --- Evaluation ----------------------------------------------------------------------------------------

$cache = @{}
function Get-Measurement {
    # The candidate's statistics in a data folder: median, min, max, cv, n, the compared value, and
    # how much of it failed: failed_runs (a group's) or failed (an over-images statistic's images).
    param([hashtable]$Candidate, [string]$Folder)
    $key = "$Folder|$($Candidate.file)"
    if (-not $cache.ContainsKey($key)) {
        $path = Join-Path $Folder "$($Candidate.file).json"
        $cache[$key] = $null
        if (Test-Path -LiteralPath $path) { $cache[$key] = Read-BenchJson -Path $path }
    }
    $document = $cache[$key]
    if ($null -eq $document) { return $null }
    if ($Candidate.ContainsKey('group')) {
        $group = $document.summary.PSObject.Properties[$Candidate.group]
        if ($null -eq $group) { return $null }
        $stats = $group.Value.PSObject.Properties[$Candidate.metric]
        if ($null -eq $stats -or $null -eq $stats.Value.median) { return $null }
        $s = $stats.Value
        $failedRuns = 0
        if ($null -ne $group.Value.PSObject.Properties['failed_runs']) { $failedRuns = [int]$group.Value.failed_runs }
        return [ordered]@{ value = $s.median; median = $s.median; min = $s.min; max = $s.max; cv = $s.cv; n = [int]$s.n
            failed_runs = $failedRuns; failed = @() }
    }
    $s = Get-BenchValue $document $Candidate.stat
    if ($null -eq $s) { return $null }
    $value = Get-BenchValue $s $Candidate.value
    if ($null -eq $value) { return $null }
    $failedImages = @()
    if ($Candidate.ContainsKey('failed')) { $failedImages = @(Get-BenchValue $document $Candidate.failed | Where-Object { $_ }) }
    return [ordered]@{ value = $value; median = $value; min = $value; max = $value
        cv = Get-BenchValue $document $Candidate.cv; n = [int](Get-BenchValue $s 'n'); failed_runs = 0; failed = $failedImages }
}

if ($Create) {
    $folder = Resolve-RepoPath $BaselineDir
    $entries = foreach ($c in $candidates) {
        $m = Get-Measurement -Candidate $c -Folder $folder
        if ($null -ne $m) { $m.Remove('failed_runs'); $m.Remove('failed') }
        $tracked = $null
        if ($c.ContainsKey('tracked')) { $tracked = $c.tracked }
        if ($null -eq $m) {
            [ordered]@{ id = $c.id; file = $c.file; source = $(if ($c.ContainsKey('group')) { "$($c.group) / $($c.metric)" } else { "$($c.stat).$($c.value)" })
                kind = $c.kind; unit = $c.unit; baseline = $null
                budget = [ordered]@{ budgeted = $false; max = $null; min = $null; reason = 'not in the baseline' } }
            continue
        }
        $budget = New-BenchBudget -Kind $c.kind -Median $m.median -Min $m.min -Max $m.max -Cv $m.cv -N $m.n -TrackedReason $tracked
        [ordered]@{ id = $c.id; file = $c.file
            source = $(if ($c.ContainsKey('group')) { "$($c.group) / $($c.metric)" } else { "$($c.stat).$($c.value)" })
            kind = $c.kind; unit = $c.unit; baseline = $m; budget = $budget }
    }
    $document = [ordered]@{
        schema = 'lucent.bench-budgets'; version = 1
        baseline = $BaselineDir.Replace('\', '/')
        rule = ('A measurement is a budget when its baseline has at least three values and a coefficient of variation ' +
            'of at most 10 % (times, rates) or 5 % (memory peaks): time max = max(1.25 x median, 1.10 x largest), ' +
            'memory max = 1.10 x largest, rate min = min(0.80 x median, 0.90 x smallest), three significant digits. ' +
            'Others are tracked. Valid on the reference machine in a like state only (machine.json).')
        budgets = @($entries)
    }
    $out = Resolve-RepoPath $Budgets
    Write-BenchJson -InputObject (ConvertTo-BenchRounded $document) -Path $out
    $budgeted = @($entries | Where-Object { $_.budget.budgeted }).Count
    Write-Host ("{0} measurements: {1} budgets, {2} tracked -> {3}" -f @($entries).Count, $budgeted, (@($entries).Count - $budgeted), $out)
    exit 0
}

if (-not $DataDir) { throw 'Give -DataDir (a new run of tools\bench\Invoke-LucentBench.ps1), or -Create.' }
$definition = Read-BenchJson -Path (Resolve-RepoPath $Budgets)
$folder = Resolve-RepoPath $DataDir
if (-not (Test-Path -LiteralPath $folder -PathType Container) -or
    @(Get-ChildItem -LiteralPath $folder -Filter '*.json' -File -ErrorAction SilentlyContinue).Count -eq 0) {
    Write-Output "No measurement was found in $folder (a run of tools\bench\Invoke-LucentBench.ps1 writes <scenario>.json there)."
    exit 2
}
# The baseline the budgets were derived from: what had already failed there is not a regression.
$baselineFolder = Resolve-RepoPath ([string]$definition.baseline).Replace('/', '\')
$failed = 0
Write-Output '| Measurement | Unit | Baseline | Budget | Measured | Result |'
Write-Output '|---|---|---|---|---|---|'
foreach ($entry in @($definition.budgets)) {
    $candidate = @($candidates | Where-Object { $_.id -eq $entry.id } | Select-Object -First 1)
    $value = $null
    $m = $null
    if ($candidate.Count -eq 1) {
        $m = Get-Measurement -Candidate $candidate[0] -Folder $folder
        if ($m) { $value = $m.value }
    }
    $result = Test-BenchBudgetValue -Budget $entry.budget -Value $value
    if ($entry.budget.budgeted) {
        # A budget is only as good as its measurement (COR-2).
        $doubts = New-Object System.Collections.ArrayList
        if ($candidate.Count -ne 1) { [void]$doubts.Add('no such measurement in this script') }
        elseif ($null -ne $m) {
            $base = Get-Measurement -Candidate $candidate[0] -Folder $baselineFolder
            $baseFailedRuns = 0
            $baseFailed = @()
            if ($null -ne $base) { $baseFailedRuns = $base.failed_runs; $baseFailed = @($base.failed) }
            if ($m.failed_runs -gt $baseFailedRuns) { [void]$doubts.Add("$($m.failed_runs) failed run(s)") }
            $newlyFailed = @($m.failed | Where-Object { $baseFailed -notcontains $_ })
            if ($newlyFailed.Count -gt 0) { [void]$doubts.Add("failed: $($newlyFailed -join ', ')") }
            if ($entry.baseline -and $m.n -lt [int]$entry.baseline.n) { [void]$doubts.Add("$($m.n) of $($entry.baseline.n) values") }
        }
        if ($doubts.Count -gt 0) {
            if ($result -eq 'ok') { $result = ($doubts -join '; ') } else { $result = "$result; " + ($doubts -join '; ') }
        }
        if ($result -ne 'ok') { $failed++ }
    }
    $limit = 'tracked'
    if ($entry.budget.budgeted) {
        if ($null -ne $entry.budget.max) { $limit = "<= $($entry.budget.max)" } else { $limit = ">= $($entry.budget.min)" }
    }
    $baseline = '-'
    if ($entry.baseline) { $baseline = [string]$entry.baseline.median }
    $measured = '-'
    if ($null -ne $value) { $measured = [string][math]::Round([double]$value, 3) }
    Write-Output ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f $entry.id, $entry.unit, $baseline, $limit, $measured, $result)
}
Write-Output ''
Write-Output ("{0} budgets failed (exceeded, missing or not trustworthy)." -f $failed)
if ($failed -gt 0) { exit 1 }
exit 0
