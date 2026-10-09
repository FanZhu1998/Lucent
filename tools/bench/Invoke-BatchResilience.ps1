<#
.SYNOPSIS
    The 1,000-file batch resilience run (blueprint section 10, "Batch resilience"; ADR 0021).

.DESCRIPTION
    1. Corpus (-WorkDir\corpus), -Count files in a fixed shuffled order, named c<NNNN>-... so that
       every output name is unique: every cached fixture once (a hard link; Lucent imports further
       hard links of a file it has as duplicates of that photo, ADR 0008, so links cannot multiply
       photos), -Copies real copies of five small fixtures, -Corrupt copies of fixtures cut to half
       their size (which must end failed or skipped with a reason, never exported), and synthetic
       DNGs for the rest (LucentBench.SyntheticDng: 0.8 to 2.7 MP, own bytes each). Full copies of
       the fixtures for 1,000 files would need about 31 GB.
    2. Session 1: Lucent.exe --export-batch <corpus> --to <out> --preset jpeg (as shot) with
       --pause-batch-after -PauseAt --resume-after 10000. While it runs, the harness ends one
       lucent-worker.exe process (an injected worker crash) once each count of -CrashAt photos is
       done, and ends Lucent.exe itself (as a power cut or a crash would) once -KillAt photos are done.
    3. Session 2: Lucent.exe --resume-batch on the same data root: recovery reconciles the attempts
       the kill interrupted and the batch runs to its end and writes its report.
    4. Session 3 (cancellation): the first -CancelSubset files of the corpus as a new batch in a new
       library, cancelled once -CancelAfter photos are done.
    Throughout, a watcher hashes every output file once it is 60 s old and removes it (1,000 JPEGs
    would not fit the disk; the newest outputs stay, so the recovery after the kill sees them), and
    the sampler records Lucent.exe's and the workers' commit every -SampleMs.

    Checks (resilience.json, "checks"): each report against the hashes the watcher recorded
    (Test-BenchBatchReport: every exported entry's file has the receipt's bytes and size, no file
    without a receipt, no source or output twice, failed and skipped entries have reasons, no
    temporary file left); one entry per corpus file; the files of one fixture (its link and its
    copies) end the same way, as do the synthetic DNGs, and
    no truncated copy is exported; the injected faults were survived (retries counted); the originals
    are unchanged; memory stays bounded (Lucent.exe's commit in the last tenth of the samples is not
    more than 64 MiB above the first tenth, and the workers' total stays under the aggregate limit).

.PARAMETER BuildDir
    Release build tree (default build\w12-perf).
.PARAMETER WorkDir
    Scratch folder (default <BuildDir>-resilience); must not be inside fixtures\, src\ or apps\.
.PARAMETER OutputDir
    Where resilience.json goes (default docs\performance\data\<today>).

.EXAMPLE
    powershell -NoProfile -File tools\bench\Invoke-BatchResilience.ps1

.NOTES
    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII. Takes about an hour on the
    reference machine (Ryzen 5 5500U).
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\w12-perf',
    [string]$WorkDir,
    [string]$OutputDir,
    [ValidateRange(10, 100000)][int]$Count = 1000,
    [ValidateRange(0, 1000)][int]$Corrupt = 10,
    [ValidateRange(0, 1000)][int]$Copies = 100,
    [string[]]$CrashAt = @('100', '450'),
    [int]$PauseAt = 250,
    [int]$KillAt = 600,
    [int]$CancelSubset = 100,
    [int]$CancelAfter = 20,
    [ValidateRange(100, 10000)][int]$SampleMs = 1000,
    [int]$HashAfterSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $PSScriptRoot 'LucentBench.psm1') -Force

# -CrashAt 100,450 arrives as one string through powershell -File (and "100,450" would read as a
# number with a thousands separator).
$CrashAt = @($CrashAt | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ } |
        ForEach-Object { [int]::Parse($_, [System.Globalization.CultureInfo]::InvariantCulture) })

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Get-RelativePath {
    param([string]$Path)
    if ($Path.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) { return $Path.Substring($repoRoot.Length).TrimStart('\') }
    return $Path
}

$BuildDir = Resolve-RepoPath $BuildDir
$lucentExe = Join-Path $BuildDir 'bin\Lucent.exe'
if (-not (Test-Path -LiteralPath $lucentExe)) { throw "Missing ${lucentExe}: build the release preset first." }
if (-not $WorkDir) { $WorkDir = "$BuildDir-resilience" } else { $WorkDir = Resolve-RepoPath $WorkDir }
foreach ($forbidden in @('fixtures', 'src', 'apps', 'docs', 'tools', 'tests')) {
    if ($WorkDir.StartsWith((Join-Path $repoRoot $forbidden), [StringComparison]::OrdinalIgnoreCase)) {
        throw "-WorkDir may not be inside $forbidden."
    }
}
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot ('docs\performance\data\' + (Get-Date -Format 'yyyy-MM-dd')) }
else { $OutputDir = Resolve-RepoPath $OutputDir }
if (Test-Path -LiteralPath $WorkDir) {
    Write-Host "Removing the previous run in $WorkDir"
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
$corpusDir = Join-Path $WorkDir 'corpus'
$cancelCorpusDir = Join-Path $WorkDir 'corpus-cancel'
$outDir = Join-Path $WorkDir 'out'
$cancelOutDir = Join-Path $WorkDir 'out-cancel'
$dataRoot = Join-Path $WorkDir 'data'
$cancelDataRoot = Join-Path $WorkDir 'data-cancel'
$logDir = Join-Path $WorkDir 'logs'
New-Item -ItemType Directory -Force -Path $corpusDir, $cancelCorpusDir, $logDir, $OutputDir | Out-Null
Stop-BenchProcessesNamed -Names @('Lucent', 'lucent-worker') -Folder $BuildDir

# ------------------------------------------------------------------------------------------------
# Corpus
# ------------------------------------------------------------------------------------------------

$fixtures = @(Get-BenchFixtures -RepoRoot $repoRoot -Tier all)
if ($fixtures.Count -eq 0) { throw 'No fixture is cached: run tools\fetch-fixtures.ps1 -Tier all first.' }
$sourceHashes = @{}
foreach ($f in $fixtures) { $sourceHashes[$f.Id] = (Get-FileHash -LiteralPath $f.Path -Algorithm SHA256).Hash }

# Lucent imports a hard link of a file it already has as a duplicate of that photo (ADR 0008), so
# only one link per fixture becomes a photo; the other files must be files of their own: real
# copies of small fixtures, truncated copies, and synthetic DNGs (LucentBench.SyntheticDng).
$smallIds = @('canon-eos-350d-cr2', 'canon-eos-r5-cr3-craw-crop-dualpixel', 'sony-dslr-a450-arw', 'dji-fc4382-dng',
    'canon-eos-5d-mark-iv-cr2-sraw')
$small = @($fixtures | Where-Object { $smallIds -contains $_.Id })
$plan = New-Object System.Collections.ArrayList
foreach ($f in $fixtures) { [void]$plan.Add(@{ kind = 'fixture'; fixture = $f }) }
for ($i = 0; $i -lt $Copies -and $small.Count -gt 0; $i++) { [void]$plan.Add(@{ kind = 'copy'; fixture = $small[$i % $small.Count] }) }
for ($i = 0; $i -lt $Corrupt; $i++) { [void]$plan.Add(@{ kind = 'truncated'; fixture = $fixtures[($i * 7 + 3) % $fixtures.Count] }) }
$sizes = @(@(1024, 768), @(1200, 800), @(1500, 1000), @(1600, 1066), @(2000, 1334))
$synthetic = $Count - $plan.Count
for ($i = 0; $i -lt $synthetic; $i++) { [void]$plan.Add(@{ kind = 'synthetic'; size = $sizes[$i % $sizes.Count]; seed = $i + 1 }) }
# A fixed shuffle, so faults and the cancelled subset meet every kind.
$random = New-Object System.Random 20261009
$order = @($plan | Sort-Object { $random.Next() })

$needed = 0L
foreach ($p in $order) {
    if ($p.kind -eq 'copy') { $needed += $p.fixture.SizeBytes }
    elseif ($p.kind -eq 'truncated') { $needed += [int64]($p.fixture.SizeBytes / 2) }
    elseif ($p.kind -eq 'synthetic') { $needed += 2L * $p.size[0] * $p.size[1] + 1024 }
}
$free = (Get-PSDrive -Name ($WorkDir.Substring(0, 1))).Free
if ($free -lt $needed + 3GB) {
    throw ("The corpus needs {0:N1} GiB and 3 GiB to spare; {1:N1} GiB are free." -f ($needed / 1GB), ($free / 1GB))
}

$files = New-Object System.Collections.ArrayList   # corpus entries: name, fixture (or ''), kind
$links = 0; $copies = 0; $truncatedCount = 0; $syntheticCount = 0; $corpusBytes = 0L
$n = 0
foreach ($p in $order) {
    $n++
    if ($p.kind -eq 'synthetic') {
        $name = 'c{0:D4}-synthetic-{1}x{2}.dng' -f $n, $p.size[0], $p.size[1]
        $corpusBytes += [LucentBench.SyntheticDng]::Write((Join-Path $corpusDir $name), $p.size[0], $p.size[1], $p.seed)
        $syntheticCount++
        [void]$files.Add([ordered]@{ name = $name; fixture = ''; kind = 'synthetic' })
        continue
    }
    $f = $p.fixture
    $extension = [System.IO.Path]::GetExtension($f.FileName)
    if ($p.kind -eq 'truncated') {
        $name = 'c{0:D4}-truncated-{1}{2}' -f $n, $f.Id, $extension
        $length = [int64][math]::Floor($f.SizeBytes / 2)
        $source = [System.IO.File]::OpenRead($f.Path)
        try {
            $target = [System.IO.File]::Create((Join-Path $corpusDir $name))
            try {
                $buffer = New-Object byte[] (1MB)
                $left = $length
                while ($left -gt 0) {
                    $read = $source.Read($buffer, 0, [int][math]::Min($buffer.Length, $left))
                    if ($read -le 0) { break }
                    $target.Write($buffer, 0, $read)
                    $left -= $read
                }
            } finally { $target.Dispose() }
        } finally { $source.Dispose() }
        $truncatedCount++
        $corpusBytes += $length
    } elseif ($p.kind -eq 'copy') {
        $name = 'c{0:D4}-copy-{1}{2}' -f $n, $f.Id, $extension
        Copy-Item -LiteralPath $f.Path -Destination (Join-Path $corpusDir $name)
        $copies++
        $corpusBytes += $f.SizeBytes
    } else {
        $name = 'c{0:D4}-{1}{2}' -f $n, $f.Id, $extension
        if ((New-BenchHardLink -Path (Join-Path $corpusDir $name) -Target $f.Path) -eq 'link') { $links++ }
    }
    [void]$files.Add([ordered]@{ name = $name; fixture = $f.Id; kind = $p.kind })
}
$ownHashes = @{}
foreach ($entry in @($files | Where-Object { $_.kind -eq 'truncated' -or $_.kind -eq 'copy' })) {
    $ownHashes[$entry.name] = (Get-FileHash -LiteralPath (Join-Path $corpusDir $entry.name) -Algorithm SHA256).Hash
}
$subset = @($files | Select-Object -First $CancelSubset)
foreach ($entry in $subset) {
    $source = Join-Path $corpusDir $entry.name
    $copy = Join-Path $cancelCorpusDir $entry.name
    [void](New-BenchHardLink -Path $copy -Target $source)  # a new library: every corpus file is its own photo
}
Write-Host ("Corpus: {0} files: {1} fixtures (hard links), {2} copies of small fixtures, {3} truncated copies, {4} synthetic DNGs; {5:N2} GiB written" -f
    $files.Count, $links, $copies, $truncatedCount, $syntheticCount, ($corpusBytes / 1GB))

# ------------------------------------------------------------------------------------------------
# The watcher and the injections (run on every tick of a session)
# ------------------------------------------------------------------------------------------------

$script:state = @{
    done = 0
    total = 0
    observed = @{}          # output name (lower case) -> sha256, size
    firstSeen = @{}         # output name -> [DateTime] first listing
    lastScan = [DateTime]::MinValue
    injections = New-Object System.Collections.ArrayList
    crashTargets = @($CrashAt | Sort-Object)
    killAt = $KillAt
    killed = $false
    folder = $outDir
    progress = New-Object System.Collections.ArrayList
    session = ''
}

function Update-OutputWatch {
    param([string]$Folder, [switch]$All)
    if (-not (Test-Path -LiteralPath $Folder)) { return }
    $now = [DateTime]::UtcNow
    $hashed = 0
    foreach ($item in @(Get-ChildItem -LiteralPath $Folder -File -Force)) {
        $name = $item.Name
        if ($name -like '~lucent-*' -or $name -like 'Lucent-batch-*-report.*') { continue }
        $key = $name.ToLowerInvariant()
        if (-not $script:state.firstSeen.ContainsKey($key)) { $script:state.firstSeen[$key] = $now }
        $age = ($now - $script:state.firstSeen[$key]).TotalSeconds
        if (-not $All -and ($age -lt $HashAfterSeconds -or $hashed -ge 25)) { continue }
        try {
            $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
            $script:state.observed[$key] = @{ sha256 = $hash; size = $item.Length }
            $hashed++
            if (-not $All) { Remove-Item -LiteralPath $item.FullName -Force }
        } catch {
            # Still held by the coordinator (publication in progress): the next scan takes it.
        }
    }
}

$onTick = {
    param($process, $lines)
    foreach ($line in @($lines)) {
        if ($line.Text -match '^lucent: batch (\d+): (\d+) of (\d+) done') {
            $script:state.done = [int]$Matches[2]
            $script:state.total = [int]$Matches[3]
            [void]$script:state.progress.Add([ordered]@{ session = $script:state.session; seconds = $line.Seconds
                    done = [int]$Matches[2] })
        }
    }
    if ($script:state.session -eq 'export') {
        while ($script:state.crashTargets.Count -gt 0 -and $script:state.done -ge $script:state.crashTargets[0]) {
            $target = $script:state.crashTargets[0]
            $script:state.crashTargets = @($script:state.crashTargets | Select-Object -Skip 1)
            $workers = @($process.LiveDescendants('lucent-worker.exe'))
            $record = [ordered]@{ kind = 'worker crash'; at_done = $script:state.done; target_done = $target
                seconds = $process.Elapsed; pid = $null; ended = $false; live_workers = $workers.Count }
            if ($workers.Count -gt 0) {
                $victim = $workers[$workers.Count - 1]
                $record.pid = $victim
                $record.ended = $process.TerminateDescendant($victim, 0xC0000005)
            }
            [void]$script:state.injections.Add($record)
            Write-Host ("  injected: worker {0} ended at {1} done ({2} live workers)" -f $record.pid, $record.at_done, $workers.Count)
        }
        if (-not $script:state.killed -and $script:state.done -ge $script:state.killAt) {
            $script:state.killed = $true
            [void]$script:state.injections.Add([ordered]@{ kind = 'Lucent.exe ended'; at_done = $script:state.done
                    seconds = $process.Elapsed; pid = $process.Id })
            Write-Host ("  injected: Lucent.exe {0} ended at {1} done" -f $process.Id, $script:state.done)
            Stop-Process -Id $process.Id -Force
        }
    }
    if (([DateTime]::UtcNow - $script:state.lastScan).TotalSeconds -ge 2) {
        $script:state.lastScan = [DateTime]::UtcNow
        Update-OutputWatch -Folder $script:state.folder
    }
}

function Invoke-Session {
    param([string]$Name, [string[]]$Arguments, [string]$Folder, [string]$Kind)
    $script:state.session = $Kind
    $script:state.folder = $Folder
    $result = Invoke-BenchProcess -FilePath $lucentExe -ArgumentList $Arguments -LogPrefix (Join-Path $logDir $Name) `
        -SampleMs $SampleMs -TickMs 500 -TimeoutSec 14400 -OnTick $onTick
    $metrics = ConvertFrom-LucentLog -Lines $result.Lines
    $resources = Get-BenchResourceSummary -Result $result
    $samples = @($result.Samples)
    Write-Host ("  session {0}: exit {1}, {2:N1} min" -f $Name, $result.ExitCode, ($result.Seconds / 60))
    return [ordered]@{ name = $Name; exit_code = $result.ExitCode; timed_out = $result.TimedOut; seconds = $result.Seconds
        log = Get-RelativePath (Join-Path $logDir "$Name.stderr.txt"); metrics = $metrics; resources = $resources
        samples = $samples; records = @($result.Records) }
}

function Get-MemoryAnalysis {
    # Lucent.exe's commit while the batch ran (from its first finished photo): no growth from the first
    # tenth of the samples to the last, and the trend (least squares) in MiB per hour.
    param([object[]]$Samples, [double]$AggregateLimitMiB, [double]$FromSeconds = 0)
    $live = @($Samples | Where-Object { $_.RootPrivateMiB -gt 0 -and $_.Seconds -ge $FromSeconds })
    if ($live.Count -lt 20) { return [ordered]@{ samples = $live.Count; bounded = $null } }
    $tenth = [math]::Max(1, [math]::Floor($live.Count / 10))
    $first = Get-BenchStatistics -Values @($live | Select-Object -First $tenth | ForEach-Object { $_.RootPrivateMiB })
    $last = Get-BenchStatistics -Values @($live | Select-Object -Last $tenth | ForEach-Object { $_.RootPrivateMiB })
    $workers = Get-BenchStatistics -Values @($live | ForEach-Object { $_.ChildrenPrivateMiB })
    $meanT = 0.0; $meanY = 0.0
    foreach ($s in $live) { $meanT += $s.Seconds; $meanY += $s.RootPrivateMiB }
    $meanT /= $live.Count; $meanY /= $live.Count
    $num = 0.0; $den = 0.0
    foreach ($s in $live) { $num += ($s.Seconds - $meanT) * ($s.RootPrivateMiB - $meanY); $den += ($s.Seconds - $meanT) * ($s.Seconds - $meanT) }
    $slope = $null
    if ($den -gt 0) { $slope = 3600.0 * $num / $den }
    $growth = $last.median - $first.median
    $bounded = $growth -le 64
    if ($AggregateLimitMiB -gt 0 -and $workers.max -gt $AggregateLimitMiB) { $bounded = $false }
    return [ordered]@{ samples = $live.Count; from_seconds = $FromSeconds
        lucent_commit_first_tenth_median_mib = $first.median; lucent_commit_last_tenth_median_mib = $last.median
        lucent_commit_growth_mib = $growth; lucent_commit_trend_mib_per_hour = $slope
        lucent_commit_max_mib = Get-BenchAggregate -Values @($live | ForEach-Object { $_.RootPrivateMiB }) -Function Max
        workers_commit_mib = $workers; aggregate_limit_mib = $AggregateLimitMiB
        system_available_min_mib = Get-BenchAggregate -Values @($live | ForEach-Object { $_.SystemAvailableMiB }) -Function Min
        bounded = $bounded
        rule = 'from the first finished photo: last-tenth median of Lucent.exe commit <= first-tenth median + 64 MiB, and the workers together <= the aggregate limit' }
}

function Get-FirstProgressSeconds {
    # When a session's batch first finished a photo (its progress lines), or 0.
    param([string]$Session)
    $points = @($script:state.progress | Where-Object { $_.session -eq $Session })
    if ($points.Count -lt 2) { return 0.0 }
    $initial = $points[0].done
    $first = @($points | Where-Object { $_.done -gt $initial } | Select-Object -First 1)
    if ($first.Count -eq 0) { return 0.0 }
    return [double]$first[0].seconds
}

function Get-Series {
    # Every n-th sample, small enough to keep in the JSON.
    param([object[]]$Samples, [int]$Points = 400)
    $all = @($Samples)
    if ($all.Count -eq 0) { return @() }
    $step = [math]::Max(1, [math]::Ceiling($all.Count / $Points))
    $series = for ($i = 0; $i -lt $all.Count; $i += $step) {
        $s = $all[$i]
        [ordered]@{ s = [math]::Round($s.Seconds, 1); lucent_mib = [math]::Round($s.RootPrivateMiB, 1)
            workers_mib = [math]::Round($s.ChildrenPrivateMiB, 1); workers = $s.Children
            cpu_tree = [math]::Round($s.TreeCpuPercent, 1); cpu_system = [math]::Round($s.SystemCpuPercent, 1)
            available_mib = [math]::Round($s.SystemAvailableMiB, 0) }
    }
    return @($series)
}

# ------------------------------------------------------------------------------------------------
# Sessions
# ------------------------------------------------------------------------------------------------

$started = Get-Date
$machine = Get-BenchMachineFacts -RepoRoot $repoRoot -BuildDir $BuildDir
$machine.idle_cpu_percent_before = Measure-BenchIdleCpu -Seconds 5

Write-Host 'Session 1: export batch with injected worker crashes, a pause and an ended Lucent.exe'
$session1 = Invoke-Session -Name 'session1-export' -Folder $outDir -Kind 'export' -Arguments @('--data-root', $dataRoot,
    '--export-batch', $corpusDir, '--to', $outDir, '--preset', 'jpeg', '--measure', '--quit-when-done',
    '--pause-batch-after', "$PauseAt", '--resume-after', '10000')
Start-Sleep -Seconds 2   # the operating system ends the workers (Job Objects) and releases the catalog

Write-Host 'Session 2: Lucent.exe started again; the batch continues'
$session2 = Invoke-Session -Name 'session2-resume' -Folder $outDir -Kind 'resume' -Arguments @('--data-root', $dataRoot,
    '--resume-batch', '--measure', '--quit-when-done')
Update-OutputWatch -Folder $outDir -All
$mainObserved = $script:state.observed.Clone()

Write-Host "Session 3: a new batch of $CancelSubset files, cancelled after $CancelAfter"
$script:state.observed = @{}
$script:state.firstSeen = @{}
$session3 = Invoke-Session -Name 'session3-cancel' -Folder $cancelOutDir -Kind 'cancel' -Arguments @('--data-root', $cancelDataRoot,
    '--export-batch', $cancelCorpusDir, '--to', $cancelOutDir, '--preset', 'jpeg', '--measure', '--quit-when-done',
    '--cancel-batch-after', "$CancelAfter")
Update-OutputWatch -Folder $cancelOutDir -All
$cancelObserved = $script:state.observed.Clone()

# ------------------------------------------------------------------------------------------------
# Checks
# ------------------------------------------------------------------------------------------------

function Get-Leftovers {
    param([string]$Folder)
    if (-not (Test-Path -LiteralPath $Folder)) { return @() }
    return @(Get-ChildItem -LiteralPath $Folder -Force | Where-Object { $_.Name -like '~lucent-*' -or $_.Name -like '*.parts' } |
        ForEach-Object { $_.Name })
}

function Get-ReportCheck {
    param([string]$Folder, [hashtable]$Observed, [int]$ExpectedEntries)
    $reportFile = @(Get-ChildItem -LiteralPath $Folder -Filter 'Lucent-batch-*-report.json' -File | Select-Object -First 1)
    if ($reportFile.Count -eq 0) {
        return @{ report = $null; check = [ordered]@{ entries = 0; outcomes = [ordered]@{}; exported = 0
                attempts = [ordered]@{}; problems = @("no report in $Folder") } }
    }
    $report = Read-BenchJson -Path $reportFile[0].FullName
    $check = Test-BenchBatchReport -Report $report -Observed $Observed -Leftovers (Get-Leftovers -Folder $Folder) `
        -ExpectedEntries $ExpectedEntries
    return @{ report = $report; check = $check }
}

$main = Get-ReportCheck -Folder $outDir -Observed $mainObserved -ExpectedEntries $files.Count
$cancel = Get-ReportCheck -Folder $cancelOutDir -Observed $cancelObserved -ExpectedEntries $subset.Count
$problems = New-Object System.Collections.ArrayList
foreach ($p in @($main.check.problems)) { [void]$problems.Add("main batch: $p") }
foreach ($p in @($cancel.check.problems)) { [void]$problems.Add("cancelled batch: $p") }

# Outcomes: every corpus file has one entry; the files of one fixture (its link and its copies) end
# the same way, and so do the synthetic DNGs; truncated copies are never exported.
$byName = @{}
foreach ($f in $files) { $byName[$f.name.ToLowerInvariant()] = $f }
$byGroup = [ordered]@{}
$byKind = [ordered]@{}
$seen = @{}
$retried = 0
if ($main.report) {
    foreach ($e in @($main.report.entries)) {
        $name = [System.IO.Path]::GetFileName([string]$e.source).ToLowerInvariant()
        if (-not $byName.ContainsKey($name)) { [void]$problems.Add("report entry for a file not in the corpus: $name"); continue }
        $seen[$name] = $true
        $entry = $byName[$name]
        $outcome = [string]$e.outcome
        if (-not $byKind.Contains($entry.kind)) { $byKind[$entry.kind] = [ordered]@{} }
        if ($byKind[$entry.kind].Contains($outcome)) { $byKind[$entry.kind][$outcome]++ } else { $byKind[$entry.kind][$outcome] = 1 }
        if ([int]$e.attempts -ge 2) { $retried++ }
        if ($entry.kind -eq 'truncated') {
            if ($outcome -eq 'exported') { [void]$problems.Add("a truncated copy was exported: $name") }
            continue
        }
        $group = $entry.fixture
        if ($entry.kind -eq 'synthetic') { $group = 'synthetic' }
        if (-not $byGroup.Contains($group)) { $byGroup[$group] = [ordered]@{} }
        if ($byGroup[$group].Contains($outcome)) { $byGroup[$group][$outcome]++ } else { $byGroup[$group][$outcome] = 1 }
    }
    foreach ($group in $byGroup.Keys) {
        if ($byGroup[$group].Count -gt 1) {
            [void]$problems.Add("files of $group ended differently: " + (($byGroup[$group].GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))
        }
    }
    $missing = @($files | Where-Object { -not $seen.ContainsKey($_.name.ToLowerInvariant()) })
    if ($missing.Count -gt 0) {
        [void]$problems.Add("$($missing.Count) corpus files have no report entry, for example $($missing[0].name)")
    }
}
$changed = @($fixtures | Where-Object { (Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash -ne $sourceHashes[$_.Id] } |
    ForEach-Object { $_.Id })
foreach ($name in $ownHashes.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $corpusDir $name) -Algorithm SHA256).Hash -ne $ownHashes[$name]) { $changed += $name }
}
foreach ($c in $changed) { [void]$problems.Add("source changed: $c") }
$injected = @($script:state.injections | Where-Object { $_.kind -eq 'worker crash' -and $_.ended }).Count
if ($injected -lt $CrashAt.Count) { [void]$problems.Add("only $injected of $($CrashAt.Count) worker crashes were injected") }
if (-not $script:state.killed) { [void]$problems.Add('Lucent.exe was never ended') }
if ($session2.exit_code -ne 0 -and $session2.exit_code -ne 4) { [void]$problems.Add("session 2 exited with $($session2.exit_code)") }
$leftWorkers = @(@($session1.records) + @($session2.records) + @($session3.records) | Where-Object { $_.EndSeconds -lt 0 })
if ($leftWorkers.Count -gt 0) { [void]$problems.Add("$($leftWorkers.Count) processes were still running after their session") }

$limit = [double](Get-BenchValue $session2.metrics 'batch_scheduler.aggregate_limit_mib')
$memory = [ordered]@{
    session1 = Get-MemoryAnalysis -Samples $session1.samples -AggregateLimitMiB $limit -FromSeconds (Get-FirstProgressSeconds 'export')
    session2 = Get-MemoryAnalysis -Samples $session2.samples -AggregateLimitMiB $limit -FromSeconds (Get-FirstProgressSeconds 'resume')
}
foreach ($key in @('session1', 'session2')) {
    if ($memory[$key].bounded -eq $false) { [void]$problems.Add("memory grew in $key") }
}

# Throughput: photos done per minute while each session ran its batch (progress lines).
function Get-Rate {
    param([object[]]$Progress)
    $points = @($Progress)
    if ($points.Count -lt 2) { return $null }
    $first = $points[0]; $last = $points[$points.Count - 1]
    if ($last.seconds -le $first.seconds) { return $null }
    return [ordered]@{ from_done = $first.done; to_done = $last.done; minutes = ($last.seconds - $first.seconds) / 60.0
        per_minute = ($last.done - $first.done) / (($last.seconds - $first.seconds) / 60.0) }
}
$progress1 = @($script:state.progress | Where-Object { $_.session -eq 'export' })
$progress2 = @($script:state.progress | Where-Object { $_.session -eq 'resume' })

$document = [ordered]@{
    schema = 'lucent.bench-resilience'; version = 1
    measured_at = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    duration_minutes = ((Get-Date) - $started).TotalMinutes
    description = ('1,000-file batch resilience run (blueprint section 10): an as-shot JPEG batch of every fixture, real ' +
        'copies of small fixtures, truncated copies and synthetic DNGs, with two injected worker crashes, a pause and ' +
        'resume, Lucent.exe ended mid-batch and started again (--resume-batch), then a separate batch cancelled after a ' +
        'few photos; every output hashed by a watcher and checked against the report.')
    parameters = [ordered]@{ count = $Count; corrupt = $Corrupt; crash_at = $CrashAt; pause_at = $PauseAt; kill_at = $KillAt
        cancel_subset = $CancelSubset; cancel_after = $CancelAfter; sample_ms = $SampleMs; hash_after_seconds = $HashAfterSeconds }
    corpus = [ordered]@{ files = $files.Count; fixtures = $links; fixture_copies = $copies; truncated_copies = $truncatedCount
        synthetic_dngs = $syntheticCount; bytes_written = $corpusBytes
        synthetic_sizes = @($sizes | ForEach-Object { '{0}x{1}' -f $_[0], $_[1] }) }
    machine = $machine
    injections = @($script:state.injections)
    checks = [ordered]@{
        passed = ($problems.Count -eq 0)
        problems = @($problems)
        main_report = [ordered]@{ entries = $main.check.entries; outcomes = $main.check.outcomes; attempts = $main.check.attempts
            outputs_hashed = $mainObserved.Count; entries_with_retries = $retried; outcomes_by_kind = $byKind
            outcomes_by_fixture = $byGroup }
        cancelled_report = [ordered]@{ entries = $cancel.check.entries; outcomes = $cancel.check.outcomes
            outputs_hashed = $cancelObserved.Count }
        sources_changed = $changed
        memory = $memory
    }
    throughput = [ordered]@{ session1 = Get-Rate $progress1; session2 = Get-Rate $progress2
        session2_timeline = Get-BenchValue $session2.metrics 'batch_timeline' }
    sessions = @()
}
foreach ($session in @($session1, $session2, $session3)) {
    $metrics = $session.metrics
    $metrics.batch_progress = @($metrics.batch_progress | Select-Object -Last 2)
    # Paths in the committed JSON are relative to the repository.
    if ($metrics.Contains('batch_report_html')) { $metrics.batch_report_html = Get-RelativePath $metrics.batch_report_html }
    if ($metrics.Contains('batch_finished')) { $metrics.batch_finished.folder = Get-RelativePath $metrics.batch_finished.folder }
    $document.sessions += , ([ordered]@{ name = $session.name; exit_code = $session.exit_code; timed_out = $session.timed_out
            seconds = $session.seconds; log = $session.log; metrics = $metrics; resources = $session.resources
            series = Get-Series -Samples $session.samples })
}
Write-BenchJson -InputObject (ConvertTo-BenchRounded $document) -Path (Join-Path $OutputDir 'resilience.json')
Write-Host ("Checks: {0}; resilience.json in {1}" -f $(if ($problems.Count -eq 0) { 'passed' } else { "$($problems.Count) problems" }),
    (Get-RelativePath $OutputDir))
foreach ($p in $problems) { Write-Host "  problem: $p" }
if ($problems.Count -gt 0) { exit 1 }
exit 0
