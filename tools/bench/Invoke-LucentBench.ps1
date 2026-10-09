<#
.SYNOPSIS
    Measures Lucent's performance baseline on a release build (docs\performance, ADR 0021).

.DESCRIPTION
    Drives the existing executables only, each run in its own process:
      Lucent.exe and its developer automation (apps\lucent\src\developer_automation.hpp), always with
        a throwaway --data-root: start-up, thumbnails, the first RAW preview, edit latency, Auto in
        the editor and its cancellation, batches through lucent-worker.exe and their cancellation;
      lucent-render.exe: one in-process export per fixture (JPEG sRGB q95 and 16-bit TIFF sRGB);
      lucent-auto.exe: the automatic analysis of every fixture.
    Every program runs under LucentBench.BenchProcess (LucentBench.psm1), which samples it and every
    process it starts (commit, working set, CPU time; system CPU and available memory) every
    -SampleMs, and keeps the exact CPU time of processes that have ended.

    Scenarios (-Scenario, default all, run in this order; each starts with one discarded warm-up run
    unless noted):
      startup        cold start: a fresh copy of the installed layout made with unbuffered I/O (none
                     of Lucent's files is in the system file cache) and a new data root; then warm
                     start: the same copy and data root again. -Repetitions pairs.
      thumbnails     first visible thumbnail after importing the smoke folder (5 photos), and every
                     fixture, into a new library (camera previews), then after a restart (disk cache).
      preview        first RAW preview (Fit) and the 100 % view of each smoke fixture, new library each time.
      edit           one-step edits (exposure, contrast) and slider drags (exposure; shadows, the guided
                     filter) on the 45 MP Canon EOS R5 and the 24 MP Nikon Z 6.
      auto-editor    Auto in the editor (analysis and full-resolution final check) on each smoke fixture.
      auto-cancel    Auto on the R5 cancelled 1 s (analysis) and 4 s (final check) after it started.
      auto-analysis  lucent-auto --group-by none over every fixture: the analysis alone, per image.
      export         lucent-render: every fixture to JPEG and to 16-bit TIFF.
      batch          batches through the worker processes: the smoke tier to JPEG, to 16-bit TIFF and
                     to JPEG with automatic settings; every fixture to JPEG, and with automatic settings.
                     Every report is checked against the exported files (Test-BenchBatchReport).
      batch-cancel   a running batch cancelled: every fixture to JPEG after 2 photos, the smoke tier to
                     16-bit TIFF after 1.

    Cache state: the fixtures are read (hashed) once before the first scenario, so inputs come from
    the file cache unless memory pressure evicted them; Lucent's binaries are warm except in the cold
    start runs. The machine should be otherwise idle and on AC power: machine.json records what it was.

    Outputs: <OutputDir>\machine.json and <OutputDir>\<scenario>.json ("lucent.bench-scenario" v1:
    the method, every run with its parsed measurements and resources, and per-group statistics: n,
    median, p95 by nearest rank, min, max, mean, stdev, cv). Logs, data roots and corpora stay in
    -WorkDir; exported images are deleted once checked (disk space).

.PARAMETER BuildDir
    Release build tree (default build\w12-perf). Its bin folder holds the programs.
.PARAMETER AppDir
    Installed layout used by the start-up scenario (default <BuildDir>\package\stage, made by
    tools\package.ps1 -SkipBuild -StageOnly; else <BuildDir>\bin).
.PARAMETER WorkDir
    Scratch folder (default <BuildDir>-bench). Must not be inside fixtures\, src\ or apps\.
.PARAMETER OutputDir
    Where the JSON files go (default docs\performance\data\<today>).
.PARAMETER Scenario
    Scenarios to run, comma-separated (default all, in the order above).
.PARAMETER Repetitions
    Recorded repetitions of each measurement (default 3).
.PARAMETER SampleMs
    Sampling interval of batch, export and analysis runs (default 250 ms; latency runs are not sampled).
.PARAMETER Fixture
    Only these fixture ids (comma-separated) in the scenarios that go through fixtures (preview, edit,
    auto-editor, auto-cancel, export, and the corpora of thumbnails and batches). lucent-auto always
    analyses the whole tier.
.PARAMETER Tag
    Appended to every output file name (<scenario>-<tag>.json, machine-<tag>.json): a supplementary
    run beside a baseline. Without it, a run of some scenarios only writes machine-<scenarios>.json,
    so the full run's machine.json is kept.
.PARAMETER Quick
    One repetition and the smoke fixtures only: a check of the harness, not a measurement.

.EXAMPLE
    powershell -NoProfile -File tools\bench\Invoke-LucentBench.ps1
.EXAMPLE
    powershell -NoProfile -File tools\bench\Invoke-LucentBench.ps1 -Scenario startup,edit -Repetitions 5

.NOTES
    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII. Lucent.exe windows appear on the
    desktop while the scenarios run; do not use the machine meanwhile.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\w12-perf',
    [string]$AppDir,
    [string]$WorkDir,
    [string]$OutputDir,
    [string[]]$Scenario = @('startup', 'thumbnails', 'preview', 'edit', 'auto-editor', 'auto-cancel', 'auto-analysis',
        'export', 'batch', 'batch-cancel'),
    [ValidateRange(1, 50)][int]$Repetitions = 3,
    [ValidateRange(50, 5000)][int]$SampleMs = 250,
    [string[]]$Fixture,
    [string]$Tag,
    [switch]$Quick
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $PSScriptRoot 'LucentBench.psm1') -Force

# -Scenario a,b also arrives as one string through powershell -File.
$knownScenarios = @('startup', 'thumbnails', 'preview', 'edit', 'auto-editor', 'auto-cancel', 'auto-analysis', 'export',
    'batch', 'batch-cancel')
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($name in $Scenario) {
    if ($knownScenarios -notcontains $name) { throw "Unknown scenario '$name': use $($knownScenarios -join ', ')." }
}

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

$BuildDir = Resolve-RepoPath $BuildDir
$bin = Join-Path $BuildDir 'bin'
$lucentExe = Join-Path $bin 'Lucent.exe'
$renderExe = Join-Path $bin 'lucent-render.exe'
$autoExe = Join-Path $bin 'lucent-auto.exe'
foreach ($exe in @($lucentExe, $renderExe, $autoExe, (Join-Path $bin 'lucent-worker.exe'))) {
    if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe. Build the release preset first (tools\build.ps1 -Preset release)." }
}
if (-not $AppDir) {
    $stage = Join-Path $BuildDir 'package\stage'
    if (Test-Path -LiteralPath (Join-Path $stage 'Lucent.exe')) { $AppDir = $stage } else { $AppDir = $bin }
} else {
    $AppDir = Resolve-RepoPath $AppDir
}
if (-not $WorkDir) { $WorkDir = "$BuildDir-bench" } else { $WorkDir = Resolve-RepoPath $WorkDir }
foreach ($forbidden in @('fixtures', 'src', 'apps', 'docs', 'tools', 'tests')) {
    $folder = Join-Path $repoRoot $forbidden
    if ($WorkDir.StartsWith($folder, [StringComparison]::OrdinalIgnoreCase)) {
        throw "-WorkDir may not be inside $folder (the harness writes copies, catalogs and exports there)."
    }
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot ('docs\performance\data\' + (Get-Date -Format 'yyyy-MM-dd'))
} else {
    $OutputDir = Resolve-RepoPath $OutputDir
}
if ($Quick) { $Repetitions = 1 }
$logDir = Join-Path $WorkDir 'logs'
New-Item -ItemType Directory -Force -Path $WorkDir, $logDir, $OutputDir | Out-Null

# Leftovers of an interrupted earlier run would take memory and CPU from this one.
Stop-BenchProcessesNamed -Names @('Lucent', 'lucent-worker', 'lucent-render', 'lucent-auto') -Folder $WorkDir
Stop-BenchProcessesNamed -Names @('Lucent', 'lucent-worker', 'lucent-render', 'lucent-auto') -Folder $BuildDir

function Get-RelativePath {
    param([string]$Path)
    if ($Path.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) { return $Path.Substring($repoRoot.Length).TrimStart('\') }
    return $Path
}

# ------------------------------------------------------------------------------------------------
# Fixtures and corpora (hard links under -WorkDir; the fixtures themselves are only read)
# ------------------------------------------------------------------------------------------------

$fixtureTier = 'all'
if ($Quick) { $fixtureTier = 'smoke' }
$allFixtures = @(Get-BenchFixtures -RepoRoot $repoRoot -Tier $fixtureTier)
if ($Fixture) {
    $wanted = @($Fixture | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $allFixtures = @($allFixtures | Where-Object { $wanted -contains $_.Id })
    if ($allFixtures.Count -eq 0) { throw "None of the fixtures $($wanted -join ', ') is cached." }
}
$smokeFixtures = @($allFixtures | Where-Object { $_.Tier -eq 'smoke' })
if ($Fixture -and $smokeFixtures.Count -eq 0) { $smokeFixtures = $allFixtures }
if ($smokeFixtures.Count -eq 0) { throw 'No smoke fixture is cached: run tools\fetch-fixtures.ps1 first.' }
$suffix = ''
if ($Tag) { $suffix = '-' + ($Tag -replace '[^A-Za-z0-9_-]', '_') }
$machineFile = 'machine.json'
if ($Tag) { $machineFile = "machine$suffix.json" }
elseif (@($knownScenarios | Where-Object { $Scenario -notcontains $_ }).Count -gt 0) { $machineFile = 'machine-' + ($Scenario -join '+') + '.json' }

$corpusRoot = Join-Path $WorkDir 'corpus'
$corpus = @{ smoke = Join-Path $corpusRoot 'smoke'; all = Join-Path $corpusRoot 'all' }
$linkPath = @{}
foreach ($set in @('smoke', 'all')) {
    $members = $allFixtures
    if ($set -eq 'smoke') { $members = $smokeFixtures }
    foreach ($f in $members) {
        $path = Join-Path $corpus[$set] ("{0}\{1}" -f $f.Id, $f.FileName)
        if (-not (Test-Path -LiteralPath $path)) { [void](New-BenchHardLink -Path $path -Target $f.Path) }
        $linkPath["$set/$($f.Id)"] = $path
    }
}

Write-Host "Reading the fixtures once (file cache warm-up; source hashes before)..."
$sourceHashes = @{}
foreach ($f in $allFixtures) { $sourceHashes[$f.Id] = (Get-FileHash -LiteralPath $f.Path -Algorithm SHA256).Hash }

$fixtureFacts = @($allFixtures | ForEach-Object {
        [ordered]@{ id = $_.Id; tier = $_.Tier; make = $_.Make; model = $_.Model; file = $_.FileName
            bytes = $_.SizeBytes; width = $_.Width; height = $_.Height; megapixels = $_.Megapixels } })
$megapixels = @{}
foreach ($f in $allFixtures) { $megapixels[$f.Id] = $f.Megapixels }

$machine = Get-BenchMachineFacts -RepoRoot $repoRoot -BuildDir $BuildDir
$machine.idle_cpu_percent_before = Measure-BenchIdleCpu -Seconds 5
$machine.harness = [ordered]@{ build_dir = Get-RelativePath $BuildDir; app_dir = Get-RelativePath $AppDir
    work_dir = Get-RelativePath $WorkDir; repetitions = $Repetitions; sample_ms = $SampleMs; quick = [bool]$Quick
    powershell = $PSVersionTable.PSVersion.ToString(); scenarios = $Scenario }
$machine.fixtures = $fixtureFacts
Write-BenchJson -InputObject (ConvertTo-BenchRounded $machine) -Path (Join-Path $OutputDir $machineFile)

# ------------------------------------------------------------------------------------------------
# Runs
# ------------------------------------------------------------------------------------------------

function New-DataRoot {
    param([string]$Scenario, [string]$Name)
    $path = Join-Path $WorkDir ("data\{0}\{1}" -f $Scenario, $Name)
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    return $path
}

function Invoke-Lucent {
    param(
        [string]$Scenario, [string]$Name, [int]$Repetition, [string[]]$Arguments,
        [string]$Exe = $lucentExe, [int]$TimeoutSec = 600, [int]$Sample = 0, [switch]$Warmup, [switch]$Screenshot,
        [scriptblock]$OnTick
    )
    $tag = '{0}-r{1}' -f $Name, $Repetition
    if ($Warmup) { $tag = "$Name-warmup" }
    $prefix = Join-Path $logDir "$Scenario\$tag"
    $arguments = @($Arguments)
    $shot = $null
    if ($Screenshot) {
        $shot = Join-Path $WorkDir "shots\$Scenario-$tag.png"
        $arguments += @('--screenshot', $shot, '--quit-after-screenshot', '--settle-timeout', '60000')
    }
    $result = Invoke-BenchProcess -FilePath $Exe -ArgumentList $arguments -LogPrefix $prefix -TimeoutSec $TimeoutSec `
        -SampleMs $Sample -OnTick $OnTick
    if ($shot -and (Test-Path -LiteralPath $shot)) { Remove-Item -LiteralPath $shot -Force }
    $metrics = ConvertFrom-LucentLog -Lines $result.Lines
    foreach ($edit in @($metrics.edits)) {
        $metrics['edit_' + $edit.label] = $edit.shown_ms
        $metrics['edit_' + $edit.label + '_published'] = $edit.last_published_ms
    }
    $record = [ordered]@{
        name = $Name; repetition = $Repetition; warmup = [bool]$Warmup
        exit_code = $result.ExitCode; timed_out = $result.TimedOut; seconds = $result.Seconds
        log = Get-RelativePath "$prefix.stderr.txt"
        metrics = $metrics
        resources = Get-BenchResourceSummary -Result $result
    }
    $flag = ''
    if ($result.TimedOut) { $flag = ' TIMED OUT' } elseif ($metrics.errors.Count -gt 0) { $flag = ' (' + ($metrics.errors -join '; ') + ')' }
    Write-Host ("  {0,-14} {1,-34} exit {2,-3} {3,7:N1} s{4}" -f $Scenario, $tag, $result.ExitCode, $result.Seconds, $flag)
    return $record
}

function Save-Scenario {
    param([string]$Name, [string]$Description, [object[]]$Runs, [string[]]$Paths, $Extra = $null)
    $recorded = @($Runs | Where-Object { -not $_.warmup })
    $groups = [ordered]@{}
    foreach ($run in $recorded) {
        $key = [string]$run.name
        if (-not $groups.Contains($key)) { $groups[$key] = New-Object System.Collections.ArrayList }
        [void]$groups[$key].Add($run)
    }
    $summary = [ordered]@{}
    foreach ($key in $groups.Keys) {
        # A failed run: timed out, a tool error, or an exit code other than 0 (3: Lucent.exe saved its
        # screenshot without the window settling; 4: a batch in which photos failed).
        $failed = @($groups[$key] | Where-Object {
                $exit = Get-BenchValue $_ 'exit_code'
                (Get-BenchValue $_ 'timed_out') -eq $true -or (Get-BenchValue $_ 'status') -eq 'error' -or
                ($null -ne $exit -and @(0, 3, 4) -notcontains [int]$exit) })
        $group = [ordered]@{ runs = $groups[$key].Count; failed_runs = $failed.Count }
        foreach ($path in $Paths) {
            $group[$path] = Get-BenchStatistics -Values @($groups[$key] | ForEach-Object { Get-BenchValue $_ $path })
        }
        $summary[$key] = $group
    }
    $document = [ordered]@{
        schema = 'lucent.bench-scenario'; version = 1; scenario = $Name; description = $Description
        repetitions = $Repetitions; measured_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        summary = $summary; extra = $Extra; runs = @($Runs)
    }
    Write-BenchJson -InputObject (ConvertTo-BenchRounded $document) -Path (Join-Path $OutputDir "$Name$suffix.json")
    Write-Host "  -> $(Get-RelativePath (Join-Path $OutputDir "$Name$suffix.json"))"
}

function Get-OutputObservations {
    # Every file of an output folder but the reports: name -> sha256 and size; plus leftovers.
    param([string]$Folder)
    $observed = @{}
    $leftovers = New-Object System.Collections.ArrayList
    $bytes = 0
    if (Test-Path -LiteralPath $Folder) {
        foreach ($item in @(Get-ChildItem -LiteralPath $Folder -Force)) {
            if ($item.Name -like '~lucent-*') { [void]$leftovers.Add($item.Name); continue }
            if ($item.PSIsContainer) { continue }
            if ($item.Name -like 'Lucent-batch-*-report.*') { continue }
            $observed[$item.Name.ToLowerInvariant()] = @{ sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
                size = $item.Length }
            $bytes += $item.Length
        }
    }
    return @{ observed = $observed; leftovers = @($leftovers); bytes = $bytes }
}

function Get-BatchCheck {
    # The batch report next to the HTML path Lucent reported, checked against the output folder.
    param($Metrics, [string]$Folder)
    # The report Lucent named, or the one in the folder (the finished batch's snapshot can come before
    # the report's own event, and then Lucent.exe prints no path).
    $html = Get-BenchValue $Metrics 'batch_report_html'
    $jsonPath = $null
    if ($html) { $jsonPath = [System.IO.Path]::ChangeExtension($html, '.json') }
    elseif (Test-Path -LiteralPath $Folder) {
        $found = @(Get-ChildItem -LiteralPath $Folder -Filter 'Lucent-batch-*-report.json' -File | Select-Object -First 1)
        if ($found.Count -eq 1) { $jsonPath = $found[0].FullName }
    }
    if (-not $jsonPath) { return [ordered]@{ problems = @('no report was written') } }
    if (-not (Test-Path -LiteralPath $jsonPath)) { return [ordered]@{ problems = @("missing $jsonPath") } }
    $report = Read-BenchJson -Path $jsonPath
    $seen = Get-OutputObservations -Folder $Folder
    $check = Test-BenchBatchReport -Report $report -Observed $seen.observed -Leftovers $seen.leftovers
    $check.output_bytes = $seen.bytes
    return $check
}

# ------------------------------------------------------------------------------------------------
# Scenarios
# ------------------------------------------------------------------------------------------------

function Invoke-StartupScenario {
    $runs = New-Object System.Collections.ArrayList
    $root = Join-Path $WorkDir 'startup'
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $appExe = Join-Path $AppDir 'Lucent.exe'
    [void]$runs.Add((Invoke-Lucent -Scenario startup -Name 'warm' -Repetition 0 -Warmup -Exe $appExe -Screenshot `
                -Arguments @('--data-root', (New-DataRoot startup 'warmup'), '--measure')))
    for ($i = 1; $i -le $Repetitions; $i++) {
        $copy = Join-Path $root "app-$i"
        if (Test-Path -LiteralPath $copy) { Remove-Item -LiteralPath $copy -Recurse -Force }
        Copy-BenchTreeUnbuffered -Source $AppDir -Destination $copy
        $exe = Join-Path $copy 'Lucent.exe'
        $data = New-DataRoot startup "data-$i"
        [void]$runs.Add((Invoke-Lucent -Scenario startup -Name 'cold' -Repetition $i -Exe $exe -Screenshot `
                    -Arguments @('--data-root', $data, '--measure')))
        [void]$runs.Add((Invoke-Lucent -Scenario startup -Name 'warm' -Repetition $i -Exe $exe -Screenshot `
                    -Arguments @('--data-root', $data, '--measure')))
        Remove-Item -LiteralPath $copy -Recurse -Force
    }
    Save-Scenario -Name 'startup' -Runs $runs `
        -Description ('Lucent.exe --measure with an empty library. cold: first start of a fresh copy of the ' +
            'installed layout made with unbuffered I/O (robocopy /J) and a new data root; warm: the same copy and ' +
            'data root started again right after. window_loaded_ms: QML window created; first_frame_ms: first ' +
            'frame presented (both from process creation).') `
        -Extra ([ordered]@{ app_dir = Get-RelativePath $AppDir }) `
        -Paths @('metrics.window_loaded_ms', 'metrics.first_frame_ms', 'metrics.process.peak_working_set_mib',
            'metrics.process.peak_commit_mib', 'metrics.process.cpu_s', 'seconds')
}

function Invoke-ThumbnailScenario {
    $runs = New-Object System.Collections.ArrayList
    [void]$runs.Add((Invoke-Lucent -Scenario thumbnails -Name 'smoke-new-library' -Repetition 0 -Warmup -Screenshot `
                -Arguments @('--data-root', (New-DataRoot thumbnails 'warmup'), '--import', $corpus.smoke,
                    '--include-subfolders', '--measure')))
    $sets = @('smoke', 'all')
    if ($Quick) { $sets = @('smoke') }
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($set in $sets) {
            $data = New-DataRoot thumbnails "$set-$i"
            [void]$runs.Add((Invoke-Lucent -Scenario thumbnails -Name "$set-new-library" -Repetition $i -Screenshot `
                        -Arguments @('--data-root', $data, '--import', $corpus[$set], '--include-subfolders', '--measure')))
            [void]$runs.Add((Invoke-Lucent -Scenario thumbnails -Name "$set-restart" -Repetition $i -Screenshot `
                        -Arguments @('--data-root', $data, '--measure')))
        }
    }
    Save-Scenario -Name 'thumbnails' -Runs $runs `
        -Description ('First visible thumbnail in the contact sheet, measured from the start of the automation ' +
            '(the import request). new-library: the folder is imported into a new library (camera previews read ' +
            'from the files); restart: the same library started again (thumbnails from the disk cache).') `
        -Paths @('metrics.first_thumbnail_ms', 'metrics.window_loaded_ms', 'metrics.first_frame_ms', 'metrics.import.added',
            'metrics.process.peak_working_set_mib', 'seconds')
}

function Invoke-PreviewScenario {
    $runs = New-Object System.Collections.ArrayList
    $first = $smokeFixtures[0]
    [void]$runs.Add((Invoke-Lucent -Scenario preview -Name $first.Id -Repetition 0 -Warmup -Screenshot `
                -Arguments @('--data-root', (New-DataRoot preview 'warmup'), '--import', $linkPath["smoke/$($first.Id)"],
                    '--open-first', '--zoom', '100', '--measure')))
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($f in $smokeFixtures) {
            [void]$runs.Add((Invoke-Lucent -Scenario preview -Name $f.Id -Repetition $i -Screenshot `
                        -Arguments @('--data-root', (New-DataRoot preview "$($f.Id)-$i"), '--import',
                            $linkPath["smoke/$($f.Id)"], '--open-first', '--zoom', '100', '--measure')))
        }
    }
    Save-Scenario -Name 'preview' -Runs $runs `
        -Description ('First RAW preview: the photo is added to a new library (--import) and opened in Edit. ' +
            'first_raw_preview_ms: from the import request to the first frame rendered from the RAW data on screen ' +
            '(the camera preview shown meanwhile does not count); fit_preview.visible_ms: from opening the photo to ' +
            "that frame; engine_s / preview_s: the preview service's latest full-size render (it starts when the " +
            'photo is added); then the 100 % view (full-resolution final frame) after the zoom.') `
        -Extra ([ordered]@{ megapixels = $megapixels }) `
        -Paths @('metrics.first_raw_preview_ms', 'metrics.fit_preview.visible_ms', 'metrics.fit_preview.engine_s', 'metrics.fit_preview.preview_s',
            'metrics.zoom_preview.visible_ms', 'metrics.process.peak_working_set_mib', 'metrics.process.peak_commit_mib')
}

function Invoke-EditScenario {
    $runs = New-Object System.Collections.ArrayList
    $photos = @($smokeFixtures | Where-Object { $_.Id -in @('canon-eos-r5-cr3-raw', 'nikon-z-6-nef-lossless') })
    if ($photos.Count -eq 0) { $photos = @($smokeFixtures[0]) }
    $kinds = [ordered]@{
        exposure = @('--set', 'exposure=1', '--set', 'contrast=20', '--drag', 'exposure')
        shadows = @('--drag', 'shadows')
    }
    [void]$runs.Add((Invoke-Lucent -Scenario edit -Name "$($photos[0].Id)-exposure" -Repetition 0 -Warmup -Screenshot `
                -Arguments (@('--data-root', (New-DataRoot edit 'warmup'), '--import', $linkPath["smoke/$($photos[0].Id)"],
                        '--open-first', '--measure') + $kinds.exposure)))
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($f in $photos) {
            foreach ($kind in $kinds.Keys) {
                [void]$runs.Add((Invoke-Lucent -Scenario edit -Name "$($f.Id)-$kind" -Repetition $i -Screenshot `
                            -Arguments (@('--data-root', (New-DataRoot edit "$($f.Id)-$kind-$i"), '--import',
                                    $linkPath["smoke/$($f.Id)"], '--open-first', '--measure') + $kinds[$kind])))
            }
        }
    }
    Save-Scenario -Name 'edit' -Runs $runs `
        -Description ('Interactive edit latency in the Edit view at Fit. edit_<control>: a one-step edit (typed ' +
            'value) to its final frame shown; drag: a synthetic 60 Hz mouse drag of 60 moves over a third of the ' +
            'slider, slider position to its frame shown (median, 90th percentile, max per run), the develop ' +
            "service's share, and the UI thread's largest gap between 5 ms timer ticks.") `
        -Paths @('metrics.edit_exposure=1', 'metrics.edit_contrast=20', 'metrics.drag.shown_median_ms',
            'metrics.drag.shown_p90_ms', 'metrics.drag.shown_max_ms', 'metrics.drag.service_median_ms',
            'metrics.drag.quick', 'metrics.drag.final_after_release_ms', 'metrics.drag.ui_gap_max_ms',
            'metrics.fit_preview.visible_ms')
}

function Invoke-AutoEditorScenario {
    $runs = New-Object System.Collections.ArrayList
    $first = $smokeFixtures[0]
    [void]$runs.Add((Invoke-Lucent -Scenario auto-editor -Name $first.Id -Repetition 0 -Warmup -Screenshot `
                -Arguments @('--data-root', (New-DataRoot auto-editor 'warmup'), '--import', $linkPath["smoke/$($first.Id)"],
                    '--open-first', '--auto', '--measure')))
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($f in $smokeFixtures) {
            [void]$runs.Add((Invoke-Lucent -Scenario auto-editor -Name $f.Id -Repetition $i -Screenshot -TimeoutSec 600 `
                        -Arguments @('--data-root', (New-DataRoot auto-editor "$($f.Id)-$i"), '--import',
                            $linkPath["smoke/$($f.Id)"], '--open-first', '--auto', '--measure')))
        }
    }
    Save-Scenario -Name 'auto-editor' -Runs $runs `
        -Description ("Auto in the editor (Lucent.exe --auto): the run's time from the request to its outcome " +
            '(analysis, full-resolution final check, revisions written), in process; peak memory is the whole ' +
            "Lucent.exe process's.") `
        -Extra ([ordered]@{ megapixels = $megapixels }) `
        -Paths @('metrics.auto.ended_after_ms', 'metrics.process.peak_commit_mib', 'metrics.process.peak_working_set_mib',
            'metrics.process.cpu_s')
}

function Invoke-AutoCancelScenario {
    $runs = New-Object System.Collections.ArrayList
    $photo = @($smokeFixtures | Where-Object { $_.Id -eq 'canon-eos-r5-cr3-raw' })
    if ($photo.Count -eq 0) { $photo = @($smokeFixtures[0]) }
    $photo = $photo[0]
    $delays = @(1000, 4000)
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($delay in $delays) {
            [void]$runs.Add((Invoke-Lucent -Scenario auto-cancel -Name "$($photo.Id)-after-$delay-ms" -Repetition $i -Screenshot `
                        -Arguments @('--data-root', (New-DataRoot auto-cancel "$delay-$i"), '--import',
                            $linkPath["smoke/$($photo.Id)"], '--open-first', '--auto', '--cancel-auto-after', "$delay",
                            '--measure')))
        }
    }
    Save-Scenario -Name 'auto-cancel' -Runs $runs `
        -Description ('Auto cancelled as the Automatic settings panel does, 1 s (analysis) or 4 s (final check) after ' +
            'it started: time from the cancel request to the run ending (nothing written).') `
        -Paths @('metrics.auto_cancel.ended_after_request_ms', 'metrics.auto_cancel.progress', 'metrics.auto.cancelled',
            'metrics.auto.ended_after_ms')
}

function Invoke-AutoAnalysisScenario {
    $runs = New-Object System.Collections.ArrayList
    $items = New-Object System.Collections.ArrayList
    $tier = 'all'
    if ($Quick) { $tier = 'smoke' }
    for ($i = 1; $i -le $Repetitions; $i++) {
        $out = Join-Path $WorkDir "auto-analysis\run-$i"
        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
        $prefix = Join-Path $logDir "auto-analysis\run-r$i"
        $result = Invoke-BenchProcess -FilePath $autoExe -LogPrefix $prefix -SampleMs $SampleMs -TimeoutSec 3600 `
            -ArgumentList @('--group-by', 'none', '--tier', $tier, '--out', $out, (Join-Path $repoRoot 'fixtures\cache'))
        $runJson = Join-Path $out 'run.json'
        $totals = $null
        if (Test-Path -LiteralPath $runJson) {
            $document = Read-BenchJson -Path $runJson
            $totals = $document.totals
            foreach ($item in @($document.items)) {
                $record = [ordered]@{ name = [string]$item.name; repetition = $i; warmup = $false; status = [string]$item.status
                    analysis_seconds = $null; seconds = $null; megapixels = $megapixels[[string]$item.name]
                    seconds_per_megapixel = $null }
                if ($item.status -eq 'ok') {
                    $record.analysis_seconds = [double]$item.analysis_seconds
                    $record.seconds = [double]$item.seconds
                    if ($record.megapixels) { $record.seconds_per_megapixel = $record.analysis_seconds / $record.megapixels }
                }
                [void]$items.Add($record)
            }
        }
        [void]$runs.Add([ordered]@{ name = 'lucent-auto'; repetition = $i; warmup = $false; exit_code = $result.ExitCode
                timed_out = $result.TimedOut; seconds = $result.Seconds; log = Get-RelativePath "$prefix.stderr.txt"
                totals = $totals; resources = Get-BenchResourceSummary -Result $result })
        Write-Host ("  auto-analysis  run-r{0,-29} exit {1,-3} {2,7:N1} s" -f $i, $result.ExitCode, $result.Seconds)
        Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue
    }
    # Per image: the median of its repetitions; then the distribution over images.
    $perImage = [ordered]@{}
    foreach ($group in @($items | Where-Object { $_.status -eq 'ok' } | Group-Object -Property { $_.name })) {
        $perImage[$group.Name] = [ordered]@{
            megapixels = $megapixels[$group.Name]
            analysis_seconds = Get-BenchStatistics -Values @($group.Group | ForEach-Object { $_.analysis_seconds })
            seconds_per_megapixel = Get-BenchStatistics -Values @($group.Group | ForEach-Object { $_.seconds_per_megapixel })
        }
    }
    $medians = @($perImage.Values | ForEach-Object { $_.analysis_seconds.median })
    $perMp = @($perImage.Values | ForEach-Object { $_.seconds_per_megapixel.median })
    $cvs = @($perImage.Values | ForEach-Object { $_.analysis_seconds.cv })
    $failedItems = @($items | Where-Object { $_.status -ne 'ok' } | ForEach-Object { $_.name } | Sort-Object -Unique)
    $extra = [ordered]@{
        over_images = [ordered]@{ analysis_seconds = Get-BenchStatistics -Values $medians
            seconds_per_megapixel = Get-BenchStatistics -Values $perMp
            repetition_cv = Get-BenchStatistics -Values $cvs }
        failed_images = $failedItems
        per_image = $perImage
        items = @($items)
    }
    Save-Scenario -Name 'auto-analysis' -Runs $runs -Extra $extra `
        -Description ('lucent-auto --group-by none over every fixture (Natural v1 analysis: decode, source analysis, ' +
            'proxy search, native-crop recheck; no full-resolution final check, no export). analysis_seconds is the ' +
            "analysis' own total per image; extra.over_images summarises the per-image medians.") `
        -Paths @('seconds', 'resources.root.peak_commit_mib', 'resources.root.peak_working_set_mib',
            'resources.cpu_percent_of_machine', 'totals.analysis_seconds')
}

function Invoke-ExportScenario {
    $runs = New-Object System.Collections.ArrayList
    $out = Join-Path $WorkDir 'export\out'
    $reports = Join-Path $WorkDir 'export\reports'
    New-Item -ItemType Directory -Force -Path $out, $reports | Out-Null
    $formats = @('jpeg', 'tiff16')
    $plan = New-Object System.Collections.ArrayList
    [void]$plan.Add(@{ fixture = $smokeFixtures[0]; format = 'jpeg'; repetition = 0; warmup = $true })
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($f in $allFixtures) {
            foreach ($format in $formats) { [void]$plan.Add(@{ fixture = $f; format = $format; repetition = $i; warmup = $false }) }
        }
    }
    foreach ($step in $plan) {
        $f = $step.fixture
        $tag = '{0}-{1}-r{2}' -f $step.format, $f.Id, $step.repetition
        $reportPath = Join-Path $reports "$tag.json"
        if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath -Force }
        Get-ChildItem -LiteralPath $out -Force | Remove-Item -Force -Recurse
        $prefix = Join-Path $logDir "export\$tag"
        $arguments = @($f.Path, '--out', $out, '--format', $step.format, '--report', $reportPath)
        $result = Invoke-BenchProcess -FilePath $renderExe -LogPrefix $prefix -SampleMs $SampleMs -TimeoutSec 900 `
            -ArgumentList $arguments
        $line = @($result.OutputLines | Where-Object { $_.Text -like '{*' } | Select-Object -Last 1)
        if ($line.Count -eq 1 -and $line[0].Text -match '"where":"raw\.libraw\.subimage"') {
            # A file with several images (Dual Pixel RAW): lucent-render needs the image; Lucent exports image 0.
            if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath -Force }
            $result = Invoke-BenchProcess -FilePath $renderExe -LogPrefix $prefix -SampleMs $SampleMs -TimeoutSec 900 `
                -ArgumentList ($arguments + @('--subimage', '0'))
            $line = @($result.OutputLines | Where-Object { $_.Text -like '{*' } | Select-Object -Last 1)
        }
        $status = 'error'; $seconds = $null; $width = $null; $height = $null; $errorText = $null
        if ($line.Count -eq 1) {
            $parsed = $line[0].Text | ConvertFrom-Json
            $status = [string]$parsed.status
            if ($status -eq 'ok') {
                $seconds = [double]$parsed.seconds; $width = [double]$parsed.width; $height = [double]$parsed.height
            } elseif ($parsed.error) { $errorText = [string]$parsed.error.message }
        }
        $stageOne = $null; $stageTwo = $null; $exportSeconds = $null; $peakCommit = $null; $peakWs = $null; $outBytes = $null
        if (Test-Path -LiteralPath $reportPath) {
            $report = Read-BenchJson -Path $reportPath
            $stageOne = Get-BenchValue $report 'render.stage_one.seconds'
            $stageTwo = Get-BenchValue $report 'render.stage_two_seconds'
            $exportSeconds = Get-BenchValue $report 'export.seconds'
            $outBytes = Get-BenchValue $report 'export.byte_size'
            $commit = Get-BenchValue $report 'process.peak_commit_bytes'
            $ws = Get-BenchValue $report 'process.peak_working_set_bytes'
            if ($null -ne $commit) { $peakCommit = [double]$commit / 1MB }
            if ($null -ne $ws) { $peakWs = [double]$ws / 1MB }
        }
        $mp = $null
        if ($width -and $height) { $mp = $width * $height / 1e6 }
        $perMp = $null
        if ($seconds -and $mp) { $perMp = $seconds / $mp }
        [void]$runs.Add([ordered]@{ name = "$($step.format)/$($f.Id)"; format = $step.format; fixture = $f.Id
                repetition = $step.repetition; warmup = $step.warmup; exit_code = $result.ExitCode
                timed_out = $result.TimedOut; status = $status
                error = $errorText; seconds = $seconds; process_seconds = $result.Seconds; width = $width; height = $height
                output_megapixels = $mp; seconds_per_megapixel = $perMp; render_stage_one_s = $stageOne
                render_stage_two_s = $stageTwo; export_s = $exportSeconds; output_bytes = $outBytes
                peak_commit_mib = $peakCommit; peak_working_set_mib = $peakWs; log = Get-RelativePath "$prefix.stderr.txt" })
        Write-Host ("  export         {0,-48} {1,-5} {2,7:N2} s" -f $tag, $status, $seconds)
        Get-ChildItem -LiteralPath $out -Force | Remove-Item -Force -Recurse
    }
    $recorded = @($runs | Where-Object { -not $_.warmup -and $_.status -eq 'ok' })
    $overImages = [ordered]@{}
    foreach ($format in $formats) {
        $perImage = @($recorded | Where-Object { $_.format -eq $format } | Group-Object -Property { $_.fixture } | ForEach-Object {
                [ordered]@{ fixture = $_.Name
                    seconds = (Get-BenchStatistics -Values @($_.Group | ForEach-Object { $_.seconds })).median
                    seconds_per_megapixel = (Get-BenchStatistics -Values @($_.Group | ForEach-Object { $_.seconds_per_megapixel })).median
                    cv = (Get-BenchStatistics -Values @($_.Group | ForEach-Object { $_.seconds })).cv
                    peak_commit_mib = (Get-BenchStatistics -Values @($_.Group | ForEach-Object { $_.peak_commit_mib })).max } })
        $overImages[$format] = [ordered]@{
            images = $perImage.Count
            seconds = Get-BenchStatistics -Values @($perImage | ForEach-Object { $_.seconds })
            seconds_per_megapixel = Get-BenchStatistics -Values @($perImage | ForEach-Object { $_.seconds_per_megapixel })
            repetition_cv = Get-BenchStatistics -Values @($perImage | ForEach-Object { $_.cv })
            peak_commit_mib = Get-BenchStatistics -Values @($perImage | ForEach-Object { $_.peak_commit_mib })
            failed = @($runs | Where-Object { -not $_.warmup -and $_.format -eq $format -and $_.status -ne 'ok' } |
                    ForEach-Object { $_.fixture } | Sort-Object -Unique)
        }
    }
    Save-Scenario -Name 'export' -Runs $runs -Extra ([ordered]@{ over_images = $overImages }) `
        -Description ('lucent-render <raw> --out <dir> --format jpeg|tiff16 --report: decode, export render ' +
            '(develop pipeline, as shot), encode, write, re-read and verify, in one process (no worker); seconds is ' +
            "the tool's own total, peak memory the process's. extra.over_images summarises the per-image medians.") `
        -Paths @('seconds', 'seconds_per_megapixel', 'render_stage_one_s', 'render_stage_two_s', 'export_s',
            'peak_commit_mib', 'output_bytes')
}

function Invoke-BatchRun {
    param([string]$Scenario, [string]$Name, [int]$Repetition, [string]$Corpus, [string]$Preset, [bool]$Automatic,
        [string[]]$Extra = @(), [switch]$Warmup)
    $tag = "$Name-$Repetition"
    $out = Join-Path $WorkDir "$Scenario\out-$tag"
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
    $arguments = @('--data-root', (New-DataRoot $Scenario $tag), '--export-batch', $Corpus, '--include-subfolders', '--to', $out,
        '--preset', $Preset, '--measure', '--quit-when-done')
    if ($Automatic) { $arguments += @('--automatic', 'auto') }
    $arguments += $Extra
    $record = Invoke-Lucent -Scenario $Scenario -Name $Name -Repetition $Repetition -Warmup:$Warmup -Sample $SampleMs `
        -TimeoutSec 5400 -Arguments $arguments
    $check = Get-BatchCheck -Metrics $record.metrics -Folder $out
    $record.report_check = $check
    # Paths in the committed JSON are relative to the repository.
    if ($record.metrics.Contains('batch_report_html')) { $record.metrics.batch_report_html = Get-RelativePath $record.metrics.batch_report_html }
    if ($record.metrics.Contains('batch_finished')) { $record.metrics.batch_finished.folder = Get-RelativePath $record.metrics.batch_finished.folder }
    $timeline = $record.metrics['batch_timeline']
    $derived = [ordered]@{ submission_s = $null; seconds_per_exported = $null }
    if ($timeline) {
        $derived.submission_s = ([double]$timeline.created_ms - [double]$timeline.submit_ms) / 1000.0
        if ([double]$timeline.exported -gt 0) { $derived.seconds_per_exported = [double]$timeline.seconds / [double]$timeline.exported }
    }
    $record.derived = $derived
    $record.metrics.batch_progress = @($record.metrics.batch_progress | Select-Object -Last 3)
    if (@($check.problems).Count -gt 0) { Write-Host ("    report check: " + ($check.problems -join '; ')) }
    Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue
    return $record
}

$batchPaths = @('metrics.batch_timeline.seconds', 'metrics.batch_timeline.per_minute', 'derived.seconds_per_exported',
    'derived.submission_s', 'metrics.batch_timeline.exported', 'metrics.batch_timeline.failed', 'metrics.batch_timeline.skipped',
    'metrics.batch_scheduler.peak_aggregate_commit_mib', 'metrics.batch_scheduler.peak_worker_commit_mib',
    'metrics.batch_scheduler.peak_active_workers', 'metrics.batch_scheduler.worker_starts',
    'metrics.batch_scheduler.deferred_for_memory', 'metrics.batch_scheduler.bytes_per_pixel',
    'metrics.process.peak_working_set_mib', 'metrics.process.peak_commit_mib', 'resources.cpu_percent_of_machine',
    'resources.sampled.descendants_private_max_mib', 'resources.sampled.tree_private_max_mib',
    'resources.sampled.system_cpu_percent_mean', 'resources.sampled.system_available_min_mib',
    'metrics.ui.gap_max_ms', 'report_check.output_bytes')

function Invoke-BatchScenario {
    $runs = New-Object System.Collections.ArrayList
    $configs = @(
        @{ name = 'smoke-jpeg'; corpus = $corpus.smoke; preset = 'jpeg'; automatic = $false; repetitions = $Repetitions },
        @{ name = 'smoke-tiff16'; corpus = $corpus.smoke; preset = 'tiff'; automatic = $false; repetitions = $Repetitions },
        @{ name = 'smoke-auto-jpeg'; corpus = $corpus.smoke; preset = 'jpeg'; automatic = $true; repetitions = $Repetitions }
    )
    if (-not $Quick) {
        $configs += @(
            @{ name = 'all-jpeg'; corpus = $corpus.all; preset = 'jpeg'; automatic = $false; repetitions = $Repetitions },
            @{ name = 'all-auto-jpeg'; corpus = $corpus.all; preset = 'jpeg'; automatic = $true; repetitions = [math]::Min(2, $Repetitions) }
        )
    }
    [void]$runs.Add((Invoke-BatchRun -Scenario batch -Name 'smoke-jpeg' -Repetition 0 -Warmup -Corpus $corpus.smoke -Preset jpeg -Automatic $false))
    foreach ($config in $configs) {
        for ($i = 1; $i -le $config.repetitions; $i++) {
            [void]$runs.Add((Invoke-BatchRun -Scenario batch -Name $config.name -Repetition $i -Corpus $config.corpus `
                        -Preset $config.preset -Automatic $config.automatic))
        }
    }
    Save-Scenario -Name 'batch' -Runs $runs -Paths $batchPaths `
        -Description ('Lucent.exe --export-batch <folder> --include-subfolders --to <new folder> --preset jpeg|tiff ' +
            '[--automatic auto] --measure --quit-when-done, new library each run: the folder is imported and ' +
            'fingerprinted (submission_s), then the batch runs through lucent-worker.exe processes (seconds: created ' +
            'to finished; per_minute: exported photos per minute). Scheduler figures are the batch service''s own ' +
            '(peak_aggregate_commit_mib: the workers together, Job Object accounting); resources are sampled every ' +
            "$SampleMs ms. Each report is checked against the files (report_check).")
}

function Invoke-BatchCancelScenario {
    $runs = New-Object System.Collections.ArrayList
    $configs = @(
        @{ name = 'all-jpeg-cancel-after-2'; corpus = $corpus.all; preset = 'jpeg'; after = 2 },
        @{ name = 'smoke-tiff16-cancel-after-1'; corpus = $corpus.smoke; preset = 'tiff'; after = 1 }
    )
    if ($Quick) { $configs = @(@{ name = 'smoke-jpeg-cancel-after-1'; corpus = $corpus.smoke; preset = 'jpeg'; after = 1 }) }
    for ($i = 1; $i -le $Repetitions; $i++) {
        foreach ($config in $configs) {
            [void]$runs.Add((Invoke-BatchRun -Scenario batch-cancel -Name $config.name -Repetition $i -Corpus $config.corpus `
                        -Preset $config.preset -Automatic $false -Extra @('--cancel-batch-after', "$($config.after)")))
        }
    }
    Save-Scenario -Name 'batch-cancel' -Runs $runs `
        -Paths @('metrics.batch_cancel.stopped_after_ms', 'metrics.batch_cancel.running', 'metrics.batch_timeline.exported',
            'metrics.batch_timeline.cancelled', 'metrics.batch_scheduler.attempts') `
        -Description ('A running batch cancelled as the Exports view does once N photos are done and another is ' +
            'being processed (--cancel-batch-after): stopped_after_ms runs from the request to the batch shown ' +
            'cancelled with nothing running (the interface applies batch events at most every 200 ms). The report is ' +
            'checked: exported photos have their files, cancelled ones none, no temporary file is left.')
}

$started = Get-Date
foreach ($name in $Scenario) {
    Write-Host "Scenario $name"
    switch ($name) {
        'startup' { Invoke-StartupScenario }
        'thumbnails' { Invoke-ThumbnailScenario }
        'preview' { Invoke-PreviewScenario }
        'edit' { Invoke-EditScenario }
        'auto-editor' { Invoke-AutoEditorScenario }
        'auto-cancel' { Invoke-AutoCancelScenario }
        'auto-analysis' { Invoke-AutoAnalysisScenario }
        'export' { Invoke-ExportScenario }
        'batch' { Invoke-BatchScenario }
        'batch-cancel' { Invoke-BatchCancelScenario }
    }
}

# The originals must be unchanged by everything above.
$changed = @($allFixtures | Where-Object { (Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash -ne $sourceHashes[$_.Id] } |
    ForEach-Object { $_.Id })
$machine.finished_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$machine.duration_minutes = ((Get-Date) - $started).TotalMinutes
$machine.sources_changed = $changed
Write-BenchJson -InputObject (ConvertTo-BenchRounded $machine) -Path (Join-Path $OutputDir $machineFile)
if ($changed.Count -gt 0) { throw "Fixture files changed during the run: $($changed -join ', ')" }
Write-Host ("Done in {0:N1} min; JSON in {1}" -f $machine.duration_minutes, (Get-RelativePath $OutputDir))
