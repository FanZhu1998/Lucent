<#
.SYNOPSIS
    Tests of the performance harness's shared functions (tools\bench\LucentBench.psm1): command-line
    quoting, statistics, the parsers of Lucent.exe's measure lines, the batch report check and the
    process runner with its sampler; and the regression-budget gate (tools\bench\Test-BenchBudgets.ps1)
    on the committed baseline and on altered copies of it.

.DESCRIPTION
    Runs without a build and without camera fixtures (registered with CTest as
    lucent_bench_harness_tests, label "bench"). Prints one line per failed check and a summary.
    Exit code 0 when every check passed, 1 otherwise.

    Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'tools\bench\LucentBench.psm1') -Force

$script:failures = 0
$script:checks = 0

function Assert-True {
    param([bool]$Condition, [string]$What)
    $script:checks++
    if (-not $Condition) {
        $script:failures++
        Write-Host "FAILED: $What"
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$What)
    $script:checks++
    $same = $false
    if ($null -eq $Expected) { $same = $null -eq $Actual }
    elseif ($null -ne $Actual) {
        if (($Expected -is [double] -or $Expected -is [int]) -and ($Actual -is [double] -or $Actual -is [int])) {
            $same = [math]::Abs([double]$Expected - [double]$Actual) -le 1e-9
        } else {
            $same = [string]$Expected -ceq [string]$Actual
        }
    }
    if (-not $same) {
        $script:failures++
        Write-Host ("FAILED: {0}: expected '{1}', got '{2}'" -f $What, $Expected, $Actual)
    }
}

# --- Command-line quoting: CommandLineToArgvW must give the arguments back -----------------------

if (-not ('BenchTests.ArgvSplitter' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace BenchTests
{
    public static class ArgvSplitter
    {
        [DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr CommandLineToArgvW(string commandLine, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        // The arguments of "program <arguments>" (argv[0] excluded).
        public static string[] Split(string arguments)
        {
            int count;
            IntPtr argv = CommandLineToArgvW("program.exe " + arguments, out count);
            if (argv == IntPtr.Zero) { throw new InvalidOperationException("CommandLineToArgvW failed"); }
            try
            {
                string[] result = new string[count - 1];
                for (int i = 1; i < count; i++)
                {
                    result[i - 1] = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, i * IntPtr.Size));
                }
                return result;
            }
            finally { LocalFree(argv); }
        }
    }
}
'@
}

$bs = [string][char]92
$dq = [string][char]34
$cases = @(
    @('plain'),
    @('two words', 'x'),
    @(''),
    @("C:${bs}Program Files${bs}Lucent${bs}"),
    @("quote${dq}inside"),
    @("trailing${bs}${bs}"),
    @("back${bs}${dq}quote", "tab`there"),
    @('--set', 'exposure=+1', '--crop', '0.1,0.2,0.7,0.6')
)
foreach ($case in $cases) {
    $joined = ConvertTo-BenchArgumentString -Arguments $case
    $split = [BenchTests.ArgvSplitter]::Split($joined)
    Assert-Equal $case.Count $split.Count "argument count of [$($case -join '|')] via '$joined'"
    for ($i = 0; $i -lt [math]::Min($case.Count, $split.Count); $i++) {
        Assert-Equal $case[$i] $split[$i] "argument $i of '$joined'"
    }
}
Assert-Equal 'a b' (ConvertTo-BenchArgumentString -Arguments @('a', 'b')) 'simple arguments are not quoted'

# --- Statistics --------------------------------------------------------------------------------

$s = Get-BenchStatistics -Values @(3.0, 1.0, 2.0)
Assert-Equal 3 $s.n 'n of three values'
Assert-Equal 2.0 $s.median 'median of three values'
Assert-Equal 3.0 $s.p95 'p95 of three values is the largest (nearest rank)'
Assert-Equal 1.0 $s.min 'min'
Assert-Equal 3.0 $s.max 'max'
Assert-Equal 2.0 $s.mean 'mean'
Assert-Equal 1.0 $s.stdev 'sample standard deviation'
Assert-Equal 0.5 $s.cv 'coefficient of variation'
$s = Get-BenchStatistics -Values @(4, 1, 3, 2, $null)
Assert-Equal 4 $s.n 'nulls are ignored'
Assert-Equal 2.5 $s.median 'median of an even count'
$s = Get-BenchStatistics -Values @()
Assert-Equal 0 $s.n 'no values'
Assert-True ($null -eq $s.median) 'no median without values'
$s = Get-BenchStatistics -Values @(7)
Assert-True ($null -eq $s.stdev) 'no standard deviation of one value'
$twenty = [double[]](1..20)
Assert-Equal 19.0 (Get-BenchPercentile -Sorted $twenty -P 0.95) 'p95 of 1..20'
$hundred = [double[]](1..100)
Assert-Equal 95.0 (Get-BenchPercentile -Sorted $hundred -P 0.95) 'p95 of 1..100'
Assert-Equal 50.0 (Get-BenchPercentile -Sorted $hundred -P 0.5) 'p50 of 1..100'

# --- JSON indentation ------------------------------------------------------------------------------

$compact = '{"a":1,"b":[1,2,3],"c":{"d":"x,y:z \"q\" [}","e":[]},"f":[{"g":true},{"h":null}],"i":{}}'
$indented = [LucentBench.JsonText]::Indent($compact)
$expected = "{`n  `"a`": 1,`n  `"b`": [1,2,3],`n  `"c`": {`n    `"d`": `"x,y:z \`"q\`" [}`",`n    `"e`": []`n  },`n" +
    "  `"f`": [`n    {`n      `"g`": true`n    },`n    {`n      `"h`": null`n    }`n  ],`n  `"i`": {}`n}"
Assert-Equal $expected $indented 'compact JSON is indented two spaces per level, short scalar arrays inline'
$roundTrip = ($indented | ConvertFrom-Json)
Assert-Equal 'x,y:z "q" [}' $roundTrip.c.d 'strings with separators survive the indentation'
Assert-Equal 3 @($roundTrip.b).Count 'arrays survive the indentation'
$work0 = Join-Path ([System.IO.Path]::GetTempPath()) ("lucent-bench-json-" + [guid]::NewGuid().ToString('N') + '.json')
try {
    Write-BenchJson -InputObject ([ordered]@{ name = 'x'; values = @(1.5, 2.5); nested = [ordered]@{ ok = $true } }) -Path $work0
    $back = Read-BenchJson -Path $work0
    Assert-Equal 'x' $back.name 'Write-BenchJson writes readable JSON'
    Assert-Equal 2.5 $back.values[1] 'Write-BenchJson keeps arrays'
    Assert-True $back.nested.ok 'Write-BenchJson keeps nested objects'
} finally {
    Remove-Item -LiteralPath $work0 -Force -ErrorAction SilentlyContinue
}

# --- key=value fields ----------------------------------------------------------------------------

$kv = ConvertFrom-BenchKeyValues 'a=1 b=2.5 c=text d=-3 e=1e-3'
Assert-Equal 1.0 $kv.a 'integer field'
Assert-Equal 2.5 $kv.b 'decimal field'
Assert-Equal 'text' $kv.c 'text field'
Assert-Equal (-3.0) $kv.d 'negative field'
Assert-Equal 0.001 $kv.e 'exponent field'
Assert-True ($kv.a -is [double]) 'numbers become doubles'
Assert-Equal 0 (ConvertFrom-BenchKeyValues '').Count 'empty text'

# --- Lucent.exe's measure lines ------------------------------------------------------------------

$log = @(
    'lucent: camera support database 2.1.0 (qualified 2026-10-09): 40 modes, 14 verified, from C:\x',
    'lucent: measure: window loaded 812 ms after the process started',
    'lucent: measure: first frame presented 1045 ms after the process started',
    'lucent: measure: lens database loaded 260 ms after the process started',
    'lucent: import finished: 5 added (1 limited, 0 unsupported), 0 already in library, 0 found again, 0 ignored, 0 failed',
    'lucent: measure: first thumbnail visible 233 ms after the import started',
    'lucent: measure: fit preview visible 1061 ms after it was requested; latest render 1452 x 968 px: engine 0.93 s, preview 1.02 s (request to pixels)',
    'lucent: measure: first RAW preview shown 1420 ms after the import started',
    'lucent: measure: 100 % preview visible 1750 ms after it was requested',
    'lucent: measure: edit exposure=1: 1 quick and 1 final frames; first frame published 44 ms and last 52 ms after the change; last frame shown 59 ms after it',
    'lucent: measure: edit curve=0:0,0.5:0.6,1:1: 2 quick and 1 final frames; first frame published 40 ms and last 120 ms after the change; last frame shown 131 ms after it',
    'lucent: measure: edit saturation=10: 0 quick and 1 final frames; first frame published 64 ms and last 64 ms after the change; last frame shown -1 ms after it',
    'lucent: measure: drag of exposure: 60 mouse moves over 1005 ms; 21 quick and 1 final frames published; the photo is at revision 3',
    'lucent: measure: slider-to-frame (position to its frame shown): median 67 ms, 90th percentile 74 ms, max 104 ms over 21 frames; to frame published on the UI thread: median 60 ms, max 95 ms; of which the develop service (request to frame ready): median 57 ms; final frame 150 ms after the release',
    'lucent: measure: UI thread during the drag (press to final frame): 230 ticks of a 5 ms timer; gap median 5 ms, 99th percentile 9 ms, max 10 ms; 0 gaps over 50 ms, 0 over 100 ms',
    'lucent: auto: started on Canon_EOS_R5.CR3',
    'lucent: auto: cancel requested 1500 ms after the start (31 % of the run)',
    'lucent: auto: finished after 1700 ms: Auto was cancelled.',
    'lucent: measure: auto run: ended_after_ms=1612 cancelled=1',
    'lucent: measure: auto cancel: requested_after_ms=1500 ended_after_request_ms=112 cancelled=1 progress=0.312',
    'lucent: batch 7 created',
    'lucent: batch 7: 0 of 5 done (0 exported, 0 failed, 0 skipped), 5 remaining',
    'lucent: batch 7: 2 of 5 done (2 exported, 0 failed, 0 skipped), 3 remaining',
    'lucent: batch 7: cancel requested after 2 photos done, 2 running',
    'lucent: batch 7 finished (cancelled): 2 exported, 0 skipped, 0 failed, 3 cancelled, 1 recommended for review; output folder C:\out',
    'lucent: batch 7 report: C:\out\Lucent-batch-7-report.html',
    'lucent: measure: batch 7 timeline: submit_ms=900 created_ms=1500 first_done_ms=4500 finished_ms=9000 done=5 exported=2 failed=0 skipped=0 cancelled=3 seconds=7.500 per_minute=16.00',
    'lucent: measure: batch 7 cancel: requested_ms=6000 done=2 running=2 stopped_after_ms=3000',
    'lucent: measure: batch 7 scheduler: max_workers=2 peak_active_workers=2 worker_starts=2 attempts=4 completed=2 peak_aggregate_commit_mib=1040.5',
    'lucent: batch 7 report: 2 exported, 0 analysed, 0 skipped, 0 failed, 1 for review',
    'lucent: measure: UI thread over 9.8 s: 1800 ticks of a 5 ms timer; gap median 5 ms, 99th percentile 31 ms, max 88 ms; 2 gaps over 50 ms, 0 over 100 ms (window grabs excluded)',
    'lucent: measure: process: peak_working_set_mib=512.4 peak_commit_mib=480.1 commit_mib=300.0 cpu_s=12.50 user_s=11.00 kernel_s=1.50 wall_s=10.20',
    'lucent: the window did not settle within 60000 ms',
    'not a lucent line'
)
$m = ConvertFrom-LucentLog -Lines $log
Assert-Equal 812.0 $m.window_loaded_ms 'window loaded'
Assert-Equal 1045.0 $m.first_frame_ms 'first frame presented'
Assert-Equal 260.0 $m.lens_database_loaded_ms 'lens database loaded off the UI thread (W12, APL-06)'
Assert-Equal 233.0 $m.first_thumbnail_ms 'first thumbnail'
Assert-Equal 5.0 $m.import.added 'import added'
Assert-Equal 1061.0 $m.fit_preview.visible_ms 'fit preview visible'
Assert-Equal 1452.0 $m.fit_preview.width 'fit preview width'
Assert-Equal 0.93 $m.fit_preview.engine_s 'fit preview engine seconds'
Assert-Equal 1.02 $m.fit_preview.preview_s 'fit preview service seconds'
Assert-Equal 1750.0 $m.zoom_preview.visible_ms '100 % preview visible'
Assert-Equal 1420.0 $m.first_raw_preview_ms 'first RAW preview after the import'
Assert-Equal 3 $m.edits.Count 'three edits'
Assert-True ($null -eq $m.edits[2].shown_ms) 'a frame never shown has no latency (-1 becomes null)'
Assert-Equal 64.0 $m.edits[2].last_published_ms 'its published latency stays'
Assert-Equal 'exposure=1' $m.edits[0].label 'edit label'
Assert-Equal 59.0 $m.edits[0].shown_ms 'edit shown latency'
Assert-Equal 'curve=0:0,0.5:0.6,1:1' $m.edits[1].label 'curve edit label'
Assert-Equal 131.0 $m.edits[1].shown_ms 'curve edit shown latency'
Assert-Equal 'exposure' $m.drag.label 'drag label'
Assert-Equal 21.0 $m.drag.quick 'drag quick frames'
Assert-Equal 67.0 $m.drag.shown_median_ms 'drag median'
Assert-Equal 74.0 $m.drag.shown_p90_ms 'drag p90'
Assert-Equal 104.0 $m.drag.shown_max_ms 'drag max'
Assert-Equal 57.0 $m.drag.service_median_ms 'drag service median'
Assert-Equal 150.0 $m.drag.final_after_release_ms 'drag final frame'
Assert-Equal 10.0 $m.drag.ui_gap_max_ms 'drag UI gap max'
Assert-Equal 1700.0 $m.auto.finished_ms 'auto finished (poll)'
Assert-Equal 1612.0 $m.auto.ended_after_ms 'auto run ended'
Assert-Equal 1.0 $m.auto.cancelled 'auto run cancelled'
Assert-Equal 112.0 $m.auto_cancel.ended_after_request_ms 'auto cancel latency'
Assert-Equal 7.0 $m.batch_id 'batch id'
Assert-Equal 2 $m.batch_progress.Count 'progress lines'
Assert-Equal 2.0 $m.batch_progress[1].completed 'progress completed'
Assert-Equal 3.0 $m.batch_progress[1].remaining 'progress remaining'
Assert-Equal 'cancelled' $m.batch_finished.state 'batch finished state'
Assert-Equal 3.0 $m.batch_finished.cancelled 'batch cancelled count'
Assert-Equal 'C:\out\Lucent-batch-7-report.html' $m.batch_report_html 'report path'
Assert-Equal 16.0 $m.batch_timeline.per_minute 'timeline per minute'
Assert-Equal 7.5 $m.batch_timeline.seconds 'timeline seconds'
Assert-Equal 3000.0 $m.batch_cancel.stopped_after_ms 'batch cancel latency'
Assert-Equal 1040.5 $m.batch_scheduler.peak_aggregate_commit_mib 'scheduler aggregate peak'
Assert-Equal 2.0 $m.batch_report.exported 'report rows: exported'
Assert-Equal 88.0 $m.ui.gap_max_ms 'UI thread gap max'
Assert-Equal 512.4 $m.process.peak_working_set_mib 'process peak working set'
Assert-Equal 12.5 $m.process.cpu_s 'process CPU'
Assert-Equal 1 $m.errors.Count 'one failure line'
$timed = New-Object LucentBench.OutputLine
$timed.Seconds = 4.25
$timed.Text = 'lucent: batch 9: 1 of 2 done (1 exported, 0 failed, 0 skipped), 1 remaining'
$m = ConvertFrom-LucentLog -Lines @($timed)
Assert-Equal 4.25 $m.batch_progress[0].seconds 'progress line keeps its arrival time'
$m = ConvertFrom-LucentLog -Lines @('lucent: batch 3 found with work left: 600 of 1000 done, 400 remaining')
Assert-Equal 3.0 $m.batch_id 'resumed batch id'
Assert-Equal 600.0 $m.batch_found.done 'resumed batch done'

# --- Regression budgets (ADR 0021, decision 5) ------------------------------------------------------

Assert-Equal 1190.0 (Get-BenchSignificantRound 1181.25) 'three significant digits, up'
Assert-Equal 13.3 (Get-BenchSignificantRound 13.36 -Down) 'three significant digits, down'
Assert-Equal 0.952 (Get-BenchSignificantRound 0.9511) 'three significant digits below one'
$b = New-BenchBudget -Kind time -Median 945 -Min 940 -Max 960 -Cv 0.01 -N 3
Assert-True $b.budgeted 'a steady time becomes a budget'
Assert-Equal 1190.0 $b.max 'time budget: 1.25 x median when that is larger than 1.10 x the largest value'
$b = New-BenchBudget -Kind time -Median 100 -Min 90 -Max 130 -Cv 0.09 -N 3
Assert-Equal 143.0 $b.max 'time budget: 1.10 x the largest value when that is larger'
$b = New-BenchBudget -Kind time -Median 100 -Min 60 -Max 160 -Cv 0.34 -N 3
Assert-True (-not $b.budgeted) 'a time that varies more than 10 % is tracked'
Assert-True ($b.reason -match '34 %') "the reason names the variation ($($b.reason))"
$b = New-BenchBudget -Kind memory -Median 757 -Min 757 -Max 758 -Cv 0.001 -N 3
Assert-Equal 834.0 $b.max 'memory budget: 1.10 x the largest value'
$b = New-BenchBudget -Kind memory -Median 1281 -Min 1281 -Max 1496 -Cv 0.09 -N 3
Assert-True (-not $b.budgeted) 'a memory peak that varies more than 5 % is tracked'
$b = New-BenchBudget -Kind rate -Median 22.5 -Min 21.9 -Max 22.8 -Cv 0.02 -N 3
Assert-Equal 18.0 $b.min 'rate budget: 0.80 x median when that is smaller than 0.90 x the smallest value'
$b = New-BenchBudget -Kind rate -Median 10.4 -Min 10.3 -Max 10.4 -Cv 0.01 -N 2
Assert-True (-not $b.budgeted) 'two values are not enough'
$b = New-BenchBudget -Kind time -Median 216 -Min 206 -Max 231 -Cv 0.06 -N 3 -TrackedReason 'resolution'
Assert-True (-not $b.budgeted) 'a stated reason keeps a steady value tracked'
$time = New-BenchBudget -Kind time -Median 100 -Min 100 -Max 100 -Cv 0.0 -N 3
Assert-Equal 'ok' (Test-BenchBudgetValue -Budget $time -Value 120) 'within a time budget'
Assert-Equal 'over' (Test-BenchBudgetValue -Budget $time -Value 126) 'above a time budget'
$rate = New-BenchBudget -Kind rate -Median 20 -Min 20 -Max 20 -Cv 0.0 -N 3
Assert-Equal 'under' (Test-BenchBudgetValue -Budget $rate -Value 15.9) 'below a rate budget'
Assert-Equal 'ok' (Test-BenchBudgetValue -Budget $rate -Value 16.1) 'within a rate budget'
Assert-Equal 'missing' (Test-BenchBudgetValue -Budget $rate -Value $null) 'a measurement that is missing'
Assert-Equal 'tracked' (Test-BenchBudgetValue -Budget $b -Value 1e9) 'a tracked measurement never fails'

# --- Batch report check ----------------------------------------------------------------------------

function New-TestReport {
    param([object[]]$Entries, [int]$Exported)
    return [pscustomobject]@{ schema = 'lucent.batch-report'; version = 1
        summary = [pscustomobject]@{ exported = $Exported }; entries = $Entries }
}
$entries = @(
    [pscustomobject]@{ source = 'C:\in\a.cr3'; outcome = 'exported'; output = 'C:\out\a_Lucent_0.jpg'; size_bytes = 10
        sha256 = 'aa11'; reason = ''; attempts = 1 },
    [pscustomobject]@{ source = 'C:\in\b.nef'; outcome = 'exported'; output = 'C:\out\b_Lucent_0.jpg'; size_bytes = 20
        sha256 = 'bb22'; reason = ''; attempts = 2 },
    [pscustomobject]@{ source = 'C:\in\c.dng'; outcome = 'failed'; output = $null; size_bytes = 0; sha256 = $null
        reason = 'The file is damaged.'; attempts = 1 },
    [pscustomobject]@{ source = 'C:\in\d.nef'; outcome = 'skipped'; output = $null; size_bytes = 0; sha256 = $null
        reason = 'Unsupported.'; attempts = 0 }
)
$observed = @{ 'a_lucent_0.jpg' = @{ sha256 = 'AA11'; size = 10 }; 'b_lucent_0.jpg' = @{ sha256 = 'bb22'; size = 20 } }
$check = Test-BenchBatchReport -Report (New-TestReport -Entries $entries -Exported 2) -Observed $observed -ExpectedEntries 4
Assert-Equal 0 $check.problems.Count "a consistent report has no problems ($($check.problems -join '; '))"
Assert-Equal 2 $check.exported 'exported entries'
Assert-Equal 1 $check.outcomes['failed'] 'failed outcome counted'
Assert-Equal 1 $check.attempts['2'] 'attempts histogram'

$wrong = @{ 'a_lucent_0.jpg' = @{ sha256 = 'ffff'; size = 10 }; 'b_lucent_0.jpg' = @{ sha256 = 'bb22'; size = 21 }
    'extra_lucent_0.jpg' = @{ sha256 = '00'; size = 1 } }
$check = Test-BenchBatchReport -Report (New-TestReport -Entries $entries -Exported 3) -Observed $wrong `
    -Leftovers @('~lucent-0a1b2c3d-5-a1.tmp') -ExpectedEntries 5
$text = $check.problems -join "`n"
Assert-True ($text -match 'bytes differ') 'a hash mismatch is found'
Assert-True ($text -match 'size differs') 'a size mismatch is found'
Assert-True ($text -match 'without a receipt: extra_lucent_0.jpg') 'an unrecorded output is found'
Assert-True ($text -match 'left in the output folder') 'a leftover temporary file is found'
Assert-True ($text -match 'summary says 3') 'a summary that disagrees with the entries is found'
Assert-True ($text -match 'expected 5') 'a wrong entry count is found'

$duplicate = @($entries[0], $entries[0])
$check = Test-BenchBatchReport -Report (New-TestReport -Entries $duplicate -Exported 2) -Observed $observed
$text = $check.problems -join "`n"
Assert-True ($text -match 'source listed twice') 'a source listed twice is found'
Assert-True ($text -match 'output listed twice') 'an output listed twice is found'
Assert-True ($text -match 'without a receipt: b_lucent_0.jpg') 'an output no entry names is found'
$missing = @{ 'a_lucent_0.jpg' = @{ sha256 = 'aa11'; size = 10 } }
$check = Test-BenchBatchReport -Report (New-TestReport -Entries $entries -Exported 2) -Observed $missing
Assert-True (($check.problems -join "`n") -match 'never seen in the folder: b_Lucent_0.jpg') 'an exported output that never appeared is found'

# --- The process runner and its sampler ----------------------------------------------------------

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("lucent-bench-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $ticks = @{ count = 0; lines = 0 }
    $result = Invoke-BenchProcess -FilePath $cmd -LogPrefix (Join-Path $work 'echo') -SampleMs 50 -TickMs 50 `
        -ArgumentList @('/c', 'echo lucent: measure: window loaded 5 ms after the process started 1>&2 & echo to-stdout & ping -n 2 127.0.0.1 >nul & exit /b 3') `
        -OnTick { param($p, $new) $ticks.count++; $ticks.lines += @($new).Count }
    Assert-Equal 3 $result.ExitCode 'exit code'
    Assert-True (-not $result.TimedOut) 'no timeout'
    Assert-True ($result.Seconds -gt 0.5) "the run took the ping's second ($($result.Seconds) s)"
    Assert-Equal 5.0 (ConvertFrom-LucentLog -Lines $result.Lines).window_loaded_ms 'stderr lines are parsed'
    Assert-True (@($result.OutputLines | Where-Object { $_.Text -match 'to-stdout' }).Count -eq 1) 'stdout is captured'
    Assert-True ($ticks.count -ge 2) 'the tick block ran'
    Assert-Equal 1 $ticks.lines 'every stderr line reaches the tick block once'
    Assert-True ((Get-Content -LiteralPath $result.StdErrPath) -match 'window loaded') 'stderr is written to its file'
    $names = @($result.Records | ForEach-Object { $_.Name.ToLowerInvariant() })
    Assert-True ($names -contains 'cmd.exe') 'the program is recorded'
    Assert-True ($names -contains 'ping.exe') "the program's child is recorded ($($names -join ', '))"
    Assert-True (@($result.Samples).Count -ge 3) 'samples were taken'
    $summary = Get-BenchResourceSummary -Result $result
    Assert-True ($summary.root.peak_working_set_mib -gt 0) 'the program has a peak working set'
    Assert-Equal 'cmd.exe' $summary.root.name 'the root is the program'
    Assert-True (@($summary.descendants | Where-Object { $_.name -eq 'PING.EXE' -or $_.name -eq 'ping.exe' }).Count -eq 1) 'descendants are grouped by name'
    Assert-True ($summary.cpu_s -ge 0) 'CPU time is summed'

    # Without sampling there is still the final sample: the summary must cope with one.
    $quick = Invoke-BenchProcess -FilePath $cmd -LogPrefix (Join-Path $work 'quick') -ArgumentList @('/c', 'exit 0')
    $quickSummary = Get-BenchResourceSummary -Result $quick
    Assert-Equal 0 $quick.ExitCode 'an unsampled run'
    Assert-Equal 1 $quickSummary.samples 'an unsampled run has its final sample'
    Assert-True ($quickSummary.root.peak_working_set_mib -gt 0) 'peaks are read after the process ended'
    Assert-True ($null -eq $quickSummary.sampled.system_cpu_percent_mean) 'no system CPU mean from a single sample'
    Assert-Equal 2.0 (Get-BenchAggregate -Values @(1, $null, 2) -Function Max) 'aggregate max ignores nulls'
    Assert-True ($null -eq (Get-BenchAggregate -Values @() -Function Mean)) 'aggregate of nothing'

    $slow = Invoke-BenchProcess -FilePath $cmd -LogPrefix (Join-Path $work 'slow') -TimeoutSec 1 -TickMs 50 `
        -ArgumentList @('/c', 'ping -n 30 127.0.0.1 >nul')
    Assert-True $slow.TimedOut 'a run past its timeout is ended'
    Assert-True ($slow.Seconds -lt 20) "the ended run stopped early ($($slow.Seconds) s)"
    $leftover = @($slow.Records | Where-Object { $_.EndSeconds -lt 0 })
    Assert-Equal 0 $leftover.Count 'no process of an ended run is left running'

    $cpu = Measure-BenchIdleCpu -Seconds 1
    Assert-True ($cpu -ge 0 -and $cpu -le 100) "system CPU use is a percentage ($cpu)"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

# --- The regression-budget gate fails closed (tools\bench\Test-BenchBudgets.ps1; COR-2, W12) -------

$gate = Join-Path $repoRoot 'tools\bench\Test-BenchBudgets.ps1'
# The baseline budgets.json was derived from (ADR 0021; 2026-10-10 since the W13 closure).
$baselineDir = Join-Path $repoRoot 'docs\performance\data\2026-10-10'
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$gateWork = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-bench-gate-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $gateWork | Out-Null
function Invoke-Gate {
    # The baselines of 2026-10-09 and 2026-10-10 were measured before the harness waited for a quiet
    # machine, so they record no wait: -AllowBusyMachine accepts them (BENCH-2).
    param([string]$DataDir, [switch]$Strict)
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $gate, '-DataDir', $DataDir)
    if (-not $Strict) { $arguments += '-AllowBusyMachine' }
    $output = @(& $powershell @arguments)
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}
function Set-ScenarioQuiet {
    # Writes quiet_before into every scenario file of a copied data folder, as the harness does since
    # it waits before every scenario.
    param([string]$Folder, $Quiet)
    foreach ($file in @(Get-ChildItem -LiteralPath $Folder -Filter '*.json' -File | Where-Object { $_.Name -notlike 'machine*' })) {
        $document = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        if ($document.PSObject.Properties['quiet_before']) { $document.quiet_before = $Quiet }
        else { $document | Add-Member -NotePropertyName quiet_before -NotePropertyValue $Quiet }
        [System.IO.File]::WriteAllText($file.FullName, (ConvertTo-Json -InputObject $document -Depth 64), (New-Object System.Text.UTF8Encoding($false)))
    }
}
try {
    $empty = Join-Path $gateWork 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    Assert-Equal 2 (Invoke-Gate $empty).ExitCode 'a folder without measurements is not a pass'
    Assert-Equal 2 (Invoke-Gate (Join-Path $gateWork 'absent')).ExitCode 'a folder that does not exist is not a pass'
    $baseline = Invoke-Gate $baselineDir
    Assert-Equal 0 $baseline.ExitCode ('the baseline passes its own budgets (-AllowBusyMachine: it records no quiet wait): ' + ($baseline.Output | Select-Object -Last 1))
    $strict = Invoke-Gate $baselineDir -Strict
    Assert-Equal 3 $strict.ExitCode 'BENCH-2: measurements that record no quiet wait are refused without -AllowBusyMachine'
    Assert-True (@($strict.Output | Where-Object { $_ -match '^Not quiet: startup: no quiet wait recorded' }).Count -eq 1) 'BENCH-2: it names the scenario and why'

    $quietCopy = Join-Path $gateWork 'quiet'
    Copy-Item -LiteralPath $baselineDir -Destination $quietCopy -Recurse
    Set-ScenarioQuiet -Folder $quietCopy -Quiet ([pscustomobject]@{ quiet = $true; waited_s = 6.1; samples = @(2.4, 2.6, 2.5); busiest_process_percent = @(3, 2, 2) })
    Assert-Equal 0 (Invoke-Gate $quietCopy -Strict).ExitCode 'BENCH-2: scenarios that started quiet pass without -AllowBusyMachine'
    $busyFile = Join-Path $quietCopy 'startup.json'
    $busy = Get-Content -LiteralPath $busyFile -Raw | ConvertFrom-Json
    $busy.quiet_before = [pscustomobject]@{ quiet = $false; waited_s = 300.2; samples = @(9.1, 8.8, 9.4); busiest_process_percent = @(97, 99, 98) }
    [System.IO.File]::WriteAllText($busyFile, (ConvertTo-Json -InputObject $busy -Depth 64), (New-Object System.Text.UTF8Encoding($false)))
    $result = Invoke-Gate $quietCopy -Strict
    Assert-Equal 3 $result.ExitCode 'BENCH-2: a scenario that started on a busy machine is refused'
    Assert-True (@($result.Output | Where-Object { $_ -match '^Not quiet: startup: started on a busy machine .*busiest other process 97, 99, 98 %' }).Count -eq 1) 'BENCH-2: the busy start is named with its samples'
    Assert-Equal 0 (Invoke-Gate $quietCopy).ExitCode 'BENCH-2: -AllowBusyMachine accepts it'
    $busy.quiet_before = [pscustomobject]@{ quiet = $false; skipped = $true; waited_s = 0; timeout_s = 0 }
    [System.IO.File]::WriteAllText($busyFile, (ConvertTo-Json -InputObject $busy -Depth 64), (New-Object System.Text.UTF8Encoding($false)))
    $result = Invoke-Gate $quietCopy -Strict
    Assert-True ($result.ExitCode -eq 3 -and @($result.Output | Where-Object { $_ -match '^Not quiet: startup: the wait for a quiet machine was skipped' }).Count -eq 1) 'BENCH-2: a skipped wait is refused'
    $created = Join-Path $gateWork 'budgets.json'
    $create = @(& $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $gate -Create -BaselineDir $baselineDir -Budgets $created)
    Assert-True ($LASTEXITCODE -eq 3 -and -not (Test-Path -LiteralPath $created)) "BENCH-2: no budgets from a baseline without a quiet wait ($($create | Select-Object -Last 1))"

    # A scenario that did not run, an image that failed although the baseline exported it, a failed run.
    $altered = Join-Path $gateWork 'altered'
    Copy-Item -LiteralPath $baselineDir -Destination $altered -Recurse
    Remove-Item -LiteralPath (Join-Path $altered 'auto-analysis.json')
    $exportPath = Join-Path $altered 'export.json'
    $export = Get-Content -LiteralPath $exportPath -Raw | ConvertFrom-Json
    $export.extra.over_images.jpeg.failed = @(@($export.extra.over_images.jpeg.failed) + 'sony-ilce-7m4-arw-compressed')
    $export.summary.'jpeg/canon-eos-r5-cr3-raw'.failed_runs = 1
    [System.IO.File]::WriteAllText($exportPath, (ConvertTo-Json -InputObject $export -Depth 64), (New-Object System.Text.UTF8Encoding($false)))
    $result = Invoke-Gate $altered
    Assert-Equal 1 $result.ExitCode 'missing, failed and newly failing measurements fail the gate'
    Assert-Equal 1 @($result.Output | Where-Object { $_ -match '^\| analysis\.median_over_images \|.*\| missing \|$' }).Count 'a missing measurement fails'
    Assert-Equal 1 @($result.Output | Where-Object { $_ -match '^\| export\.jpeg\.p95_over_images \|.*failed: sony-ilce-7m4-arw-compressed' }).Count 'an image that newly failed fails its statistic'
    Assert-Equal 1 @($result.Output | Where-Object { $_ -match '^\| export\.jpeg\.r5 \|.*1 failed run' }).Count 'a failed run fails its group'
    Assert-Equal 0 @($result.Output | Where-Object { $_ -match '^\| export\.tiff16\.median_over_images \|' -and $_ -notmatch '\| ok \|$' }).Count 'failures the baseline had are not regressions'
}
finally {
    Remove-Item -LiteralPath $gateWork -Recurse -Force -ErrorAction SilentlyContinue
}

# --- Scenario order and the quiet machine (W12 release phase, W13 note) -------------------------

$all = @(Get-BenchScenarioOrder)
Assert-Equal 10 $all.Count 'all ten scenarios'
Assert-Equal 'startup' $all[-1] 'the cold starts run last (their fresh copies keep the antivirus busy)'
Assert-Equal 'thumbnails' $all[0] 'thumbnails first'
$picked = @(Get-BenchScenarioOrder -Requested @('startup,preview', 'edit'))
Assert-Equal 'preview|edit|startup' ($picked -join '|') 'a requested order is the fixed order'
$picked = @(Get-BenchScenarioOrder -Requested @('preview', 'preview'))
Assert-Equal 'preview' ($picked -join '|') 'each scenario once'
$threw = $false
try { Get-BenchScenarioOrder -Requested @('warp') | Out-Null } catch { $threw = $_.Exception.Message -match "Unknown scenario 'warp'" }
Assert-True $threw 'an unknown scenario is refused'
Assert-True ((Get-Content -LiteralPath (Join-Path $repoRoot 'tools\bench\Invoke-LucentBench.ps1') -Raw) -match 'Get-BenchScenarioOrder -Requested \$Scenario') 'the harness runs the scenarios in the fixed order'

# The quiet wait with a scripted sampler: busy, busy, then quiet three times in a row.
$script:cpu = New-Object System.Collections.Queue
foreach ($value in 80, 35, 4, 12, 3, 5, 2, 90) { $script:cpu.Enqueue([double]$value) }
$quiet = Wait-BenchQuiet -MaxCpuPercent 10 -Consecutive 3 -SampleMs 1 -TimeoutSeconds 60 -Sampler { param($ms) $script:cpu.Dequeue() }
Assert-True $quiet.quiet 'a quiet machine is found'
Assert-Equal '80|35|4|12|3|5|2' ($quiet.samples -join '|') 'it waits for three quiet samples in a row (12 % breaks the run)'
$busy = Wait-BenchQuiet -MaxCpuPercent 10 -Consecutive 3 -SampleMs 1 -TimeoutSeconds 1 -Sampler { param($ms) Start-Sleep -Milliseconds 50; 50 }
Assert-True (-not $busy.quiet) 'a busy machine times out'
Assert-True ($busy.waited_s -ge 1 -and $busy.waited_s -lt 10) "the wait ends at the timeout ($($busy.waited_s) s)"
Assert-True ($busy.samples.Count -ge 2) 'the busy samples are recorded'
# BENCH-1: one busy thread is 8.3 % of 12 logical processors, below the machine-wide limit; the
# busiest other process (percent of one processor) breaks the run.
$script:activity = New-Object System.Collections.Queue
foreach ($sample in @(@(8.3, 100), @(8.4, 97), @(2.5, 30), @(2.6, 5), @(2.4, 4), @(2.5, 3))) { $script:activity.Enqueue([ordered]@{ cpu = [double]$sample[0]; busiest_process_percent = [double]$sample[1] }) }
$thread = Wait-BenchQuiet -MaxCpuPercent 10 -MaxProcessPercent 25 -Consecutive 3 -SampleMs 1 -TimeoutSeconds 60 -Sampler { param($ms) $script:activity.Dequeue() }
Assert-True $thread.quiet 'BENCH-1: a machine whose busy thread stops is found quiet'
Assert-Equal '100|97|30|5|4|3' ($thread.busiest_process_percent -join '|') 'BENCH-1: one busy thread (8.3 % of the machine) and a process at 30 % of a processor are not quiet; the busiest process of each sample is recorded'
Assert-Equal 25 $thread.max_process_percent 'BENCH-1: the per-process limit is recorded'
$activity = Measure-BenchActivity -Milliseconds 200
Assert-True ($activity.cpu -ge 0 -and $activity.cpu -le 100 -and $activity.busiest_process_percent -ge 0) "BENCH-1: a sample measures the machine and its busiest other process ($([math]::Round($activity.cpu, 1)) %, $([math]::Round($activity.busiest_process_percent, 0)) % of one processor)"
$harness = Get-Content -LiteralPath (Join-Path $repoRoot 'tools\bench\Invoke-LucentBench.ps1') -Raw
Assert-True ($harness -match '\$script:scenarioQuiet = Wait-ForQuietMachine \$name' -and $harness -match 'quiet_before = \$script:scenarioQuiet') 'BENCH-2: the harness waits before every scenario and records the wait in its JSON'

Write-Host ("{0} checks, {1} failed" -f $script:checks, $script:failures)
if ($script:failures -gt 0) { exit 1 }
exit 0
