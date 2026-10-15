<#
.SYNOPSIS
    The clean-machine gate of a Lucent package in Windows Sandbox: offline, on a Windows without the
    Visual C++ runtime, Python, Qt, Git or a compiler, Setup installs Lucent, which imports, edits and
    exports, and is uninstalled (blueprint section 10, "Offline installation";
    docs\release\clean-machine.md).

.DESCRIPTION
    1. Prepares -WorkDir (new): input\ with the package's Setup and manifest, its version rehearsal
       Setup when the package has one (rehearsal\), a few smoke fixtures (fixtures\<id>\<file>) and the
       gate's scripts (scripts\: installer\sandbox\Invoke-CleanMachineGate.ps1 and
       installer\tests\Test-InstalledPackage.ps1); results\, empty; gate-request.json, what this run
       asks for (the photos and their file names, the installer's SHA-256, the version rehearsal, the
       mode, the installer test) and the host (its Windows build with the update revision, the Windows
       Sandbox app's version).
    2. Writes <WorkDir>\lucent-clean-machine.wsb (installer\sandbox\LucentSandbox.psm1): networking,
       the virtual GPU, the clipboard, printers, audio and video input off; input\ read-only at
       C:\LucentGate\input, results\ writable at C:\LucentGate\results; at logon the gate. Validates it
       against installer\sandbox\wsb-configuration.xsd (Microsoft's documented elements and values) and
       the gate's rules.
    3. Refuses to start when a Windows Sandbox is already running (it runs one at a time, and a second
       start would leave the gate unstarted; Get-LucentSandboxInstances). Starts Windows Sandbox with the
       configuration and waits: the gate must report its first step (results\progress.json) within
       -StartTimeoutMinutes, or the host stops and says why a gate may never start (another sandbox, a
       logon command that does not run); then at most -TimeoutMinutes for results\gate.json, showing
       the gate's steps. Windows Sandbox runs the logon command elevated, so no elevation prompt is
       expected (should one appear in the sandbox window, answer Yes); gate.json records the gate's
       token and Windows' UAC settings. gate.json is read with retries while the sandbox may still hold
       it (Read-CleanMachineJson). The gate then shuts the sandbox down (unless -KeepOpen), and the
       host waits for it to close.
    4. Checks gate.json against gate-request.json (Test-CleanMachineResult): every check of the gate
       present and passed, the machine clean and offline, one JPEG and one 16-bit TIFF recorded with
       its SHA-256 per photo, the photos the host gave, the package's Setup, the upgrade to the version
       rehearsal and the refused downgrade when the package has one. The gate's own verdict is not
       enough. Writes the host's verdict, <WorkDir>\host-check.json, beside gate-request.json and
       outside the folder the sandbox can write, also when the gate never started, timed out or wrote
       no result.

    Other modes:
      -NoStart        stops after 2 (start the .wsb yourself, then -Check the folder).
      -Check <folder> runs 4 on a gate folder prepared earlier.
      -Manual         prepares a sandbox whose logon opens Explorer at the input instead of the gate,
                      for the manual steps of docs\release\clean-machine.md, starts it and returns.
      -LocalRehearsal runs the gate's logon script on this computer instead (1, 2, then the script with
                      -Rehearsal, then 4): it installs Lucent for the current user into
                      <WorkDir>\profile\Programs\Lucent, with a throwaway data root, and uninstalls it;
                      Lucent must not be installed for this user. The machine is then usually not clean
                      or offline, which the result records without failing. A check of the harness, not
                      the gate.

    Windows Sandbox must be enabled once by an administrator (docs\release\clean-machine.md); this
    script never enables it, never needs elevation, and changes nothing outside -WorkDir except the
    rehearsal's per-user installation, which it removes.

.PARAMETER PackageDir
    The folder tools\package.ps1 wrote (default build\release\package): one Lucent-Setup-<v>-x64.exe
    (not -TEST-SIGNED) and its .manifest.json.
.PARAMETER Fixture
    Fixture ids of fixtures\manifest.json (cached) or RAW file paths (default: dji-fc4382-dng,
    fujifilm-x-t4-raf-lossless, nikon-z-6-nef-lossless: a DNG, an X-Trans file and a photo the lens
    database corrects). The first, by file path order, gets Auto.
.PARAMETER WorkDir
    A new folder (default build\clean-machine-<time>).
.PARAMETER TimeoutMinutes
    The longest wait for the sandbox's result (default 60).
.PARAMETER StartTimeoutMinutes
    The longest wait for the gate's first step after Windows Sandbox was started (default 5): a gate
    that has not started by then never will.
.PARAMETER MemoryMB
    The sandbox's memory (default 4096).
.PARAMETER SkipInstallerTest
    The gate leaves out Test-InstalledPackage.ps1 -RuntimeOutcomes.
.PARAMETER KeepOpen
    The sandbox stays open when the gate is done (for a look around; close it yourself).
.PARAMETER NoStart
    Prepare and validate only.
.PARAMETER Check
    Check the result of the gate folder given (see DESCRIPTION).
.PARAMETER Manual
    A sandbox for the manual steps (see DESCRIPTION).
.PARAMETER LocalRehearsal
    Run the logon script here instead of in Windows Sandbox (see DESCRIPTION).

.EXAMPLE
    powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-0.1.0\package
.EXAMPLE
    powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-0.1.0\package -LocalRehearsal
.EXAMPLE
    powershell -NoProfile -File tools\Test-CleanMachine.ps1 -Check build\clean-machine-20261010-120000

.NOTES
    Exit code 0: the gate passed (or was prepared or started as asked); 1: it failed, timed out, never
    started, or another sandbox was running; 2: Windows Sandbox is not available. Windows PowerShell 5.1
    compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [string]$PackageDir = 'build\release\package',
    [string[]]$Fixture = @('dji-fc4382-dng', 'fujifilm-x-t4-raf-lossless', 'nikon-z-6-nef-lossless'),
    [string]$WorkDir,
    [ValidateRange(5, 600)][int]$TimeoutMinutes = 60,
    [ValidateRange(1, 60)][int]$StartTimeoutMinutes = 5,
    [ValidateRange(2048, 65536)][int]$MemoryMB = 4096,
    [switch]$SkipInstallerTest,
    [switch]$KeepOpen,
    [switch]$NoStart,
    [string]$Check,
    [switch]$Manual,
    [switch]$LocalRehearsal
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $repoRoot 'installer\sandbox\LucentSandbox.psm1') -Force
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Resolve-RepoPath([string]$Path) {
    $full = if ([System.IO.Path]::IsPathRooted($Path)) { [System.IO.Path]::GetFullPath($Path) } else { [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path)) }
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\', '/') }
    return $full
}
if (@(@($NoStart, [bool]$Check, $Manual, $LocalRehearsal) | Where-Object { $_ }).Count -gt 1) { throw '-NoStart, -Check, -Manual and -LocalRehearsal exclude each other.' }
if ($StartTimeoutMinutes -gt $TimeoutMinutes) { throw '-StartTimeoutMinutes cannot be longer than -TimeoutMinutes.' }

function Get-JsonMember($Object, [string]$Name) {
    # A member of a parsed JSON object, or $null.
    if ($null -eq $Object -or $Object -isnot [System.Management.Automation.PSCustomObject]) { return $null }
    $property = $Object.PSObject.Properties[$Name]   # strict-mode safe, also for an object without members
    if ($property) { return $property.Value }
    return $null
}

# The host's verdict, <gate folder>\host-check.json: beside gate-request.json, outside the results folder
# the sandbox can write (W12 review CMG-03), and written whatever the outcome.
function Write-HostCheck([string]$Folder, [bool]$Passed, [string]$Outcome, [AllowEmptyCollection()][string[]]$Problems = @()) {
    $request = $null
    try { $request = [System.IO.File]::ReadAllText((Join-Path $Folder 'gate-request.json')) | ConvertFrom-Json } catch { $request = $null }
    $record = [ordered]@{
        checkedAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); outcome = $Outcome; passed = $Passed; problems = @($Problems)
        request = $request; host = (Get-LucentSandboxHostFacts) }
    $path = Join-Path $Folder 'host-check.json'
    [System.IO.File]::WriteAllText($path, (ConvertTo-Json -InputObject $record -Depth 6), $utf8)
    return $path
}

# Why a gate may not start in Windows Sandbox, with what this host runs (W12 review CMG-02).
function Get-StartDiagnosis {
    $facts = Get-LucentSandboxHostFacts
    $running = @(Get-LucentSandboxInstances)
    return @(
        "Windows Sandbox runs one instance at a time: a sandbox that was already open refuses a second start (running now: $(if ($running.Count) { $running -join ', ' } else { 'none found' })).",
        "The Windows Sandbox app has been reported not to run the logon command while mapped folders work (microsoft/Windows-Sandbox issue 125, app 0.8.107.0 on Windows 11 build 26200; reported fixed in a later release, then reported again): the sandbox then sits at an idle desktop. Update the Windows Sandbox app, or see docs\release\clean-machine.md, ""When the gate does not start"".",
        ("This host: {0} {1} (build {2}); Windows Sandbox: {3}{4}." -f $facts.windows, $facts.version, $facts.build,
            $(if ($facts.sandboxApp) { $facts.sandboxApp } elseif ($facts.sandboxExecutable) { "the Windows feature, WindowsSandbox.exe $($facts.sandboxExecutable)" } else { 'not found' }),
            $(if ($facts.wsbCommandLine) { ', with its command line wsb.exe' } else { '' })))
}

# 4. The result of a gate folder, checked against what was asked for rather than taken from the gate's own verdict.
function Complete-Gate([string]$Folder) {
    $request = [System.IO.File]::ReadAllText((Join-Path $Folder 'gate-request.json')) | ConvertFrom-Json
    $resultsDir = Join-Path $Folder 'results'
    $gatePath = Join-Path $resultsDir 'gate.json'
    if (-not (Test-Path -LiteralPath $gatePath -PathType Leaf)) {
        $verdict = Write-HostCheck -Folder $Folder -Passed $false -Outcome 'no result' -Problems @("$gatePath is missing: the gate wrote no result")
        Write-Host "No result: $gatePath is missing. The host's verdict: $verdict" -ForegroundColor Red
        exit 1
    }
    try { $gate = Read-CleanMachineJson -Path $gatePath -TimeoutSeconds 120 }
    catch {
        $verdict = Write-HostCheck -Folder $Folder -Passed $false -Outcome 'unreadable result' -Problems @($_.Exception.Message)
        Write-Host "$($_.Exception.Message) The host's verdict: $verdict" -ForegroundColor Red
        exit 1
    }
    $rehearsed = (Get-JsonMember $request 'mode') -eq 'rehearsal'
    $result = Test-CleanMachineResult -Gate $gate -Photos ([int](Get-JsonMember $request 'photos')) -Fixtures @(Get-JsonMember $request 'fixtures') `
        -InstallerSha256 ([string](Get-JsonMember $request 'installerSha256')) -VersionRehearsal:([bool](Get-JsonMember $request 'versionRehearsal')) `
        -Rehearsal:$rehearsed -SkipInstallerTest:([bool](Get-JsonMember $request 'skipInstallerTest'))
    $verdict = Write-HostCheck -Folder $Folder -Passed $result.Passed -Outcome 'checked' -Problems @($result.Problems)
    foreach ($problem in $result.Problems) { Write-Host "  PROBLEM: $problem" -ForegroundColor Red }
    foreach ($note in @(Get-JsonMember $gate 'notes')) { if ($note) { Write-Host "  NOTE: $note" -ForegroundColor Yellow } }
    if (Test-Path -LiteralPath (Join-Path $resultsDir 'host-check.json')) {
        Write-Host "  NOTE: $(Join-Path $resultsDir 'host-check.json') is not the host's verdict (an older harness wrote it there, or the sandbox did); the host's verdict is $verdict." -ForegroundColor Yellow
    }
    $environment = Get-JsonMember $gate 'environment'
    $elevation = Get-JsonMember $environment 'elevation'
    if ($elevation) {
        $uac = Get-JsonMember $elevation 'uac'
        Write-Host ("  The gate's token: Administrators role {0}, integrity {1}, elevation type {2}; UAC: EnableLUA {3}, ConsentPromptBehaviorAdmin {4}; the runtime installer's elevation: {5}" -f
            (Get-JsonMember $elevation 'administratorsRole'), (Get-JsonMember $elevation 'integrityLevel'), (Get-JsonMember $elevation 'elevationType'),
            (Get-JsonMember $uac 'EnableLUA'), (Get-JsonMember $uac 'ConsentPromptBehaviorAdmin'), (Get-JsonMember $elevation 'runtimePrompt'))
    }
    $checks = @(Get-JsonMember $gate 'checks')
    Write-Host ("Clean-machine gate{0}: {1}. {2} of {3} checks passed; machine clean: {4}, offline: {5}. Results: {6}; the host's verdict: {7}" -f
        $(if ($rehearsed) { ' (local rehearsal: not the gate itself)' } else { '' }), $(if ($result.Passed) { 'PASSED' } else { 'FAILED' }),
        @($checks | Where-Object { Get-JsonMember $_ 'passed' }).Count, $checks.Count,
        $(if ($environment) { Get-JsonMember $environment 'clean' } else { 'unknown' }), $(if ($environment) { Get-JsonMember (Get-JsonMember $environment 'network') 'offline' } else { 'unknown' }),
        $resultsDir, $verdict) -ForegroundColor $(if ($result.Passed) { 'Green' } else { 'Red' })
    if ($result.Passed) { exit 0 }
    exit 1
}

if ($Check) { Complete-Gate (Resolve-RepoPath $Check) }

# --- 1. The gate folder ------------------------------------------------------------------------------

$package = Resolve-RepoPath $PackageDir
if (-not (Test-Path -LiteralPath $package -PathType Container)) { throw "The package folder $package does not exist." }
$installers = @(Get-ChildItem -LiteralPath $package -File -Filter 'Lucent-Setup-*-x64.exe' | Where-Object { $_.Name -notmatch '-TEST-SIGNED|-VERSION-REHEARSAL' })
if ($installers.Count -ne 1) { throw "$package must hold exactly one Lucent-Setup-<version>-x64.exe (found $($installers.Count)): run tools\package.ps1 without -StageOnly." }
$installer = $installers[0]
$manifest = Join-Path $package ($installer.BaseName + '.manifest.json')
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "The package manifest $manifest is missing." }
$manifestData = [System.IO.File]::ReadAllText($manifest) | ConvertFrom-Json
$installerSha256 = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not $manifestData.installer -or $manifestData.installer.sha256 -ne $installerSha256) {
    throw "$($installer.Name) is not the installer its package manifest records (SHA-256 $installerSha256)."
}
$rehearsal = @(Get-ChildItem -LiteralPath (Join-Path $package 'rehearsal') -File -Filter 'Lucent-Setup-*-x64-VERSION-REHEARSAL.exe' -ErrorAction SilentlyContinue)

$fixtureManifest = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'fixtures\manifest.json')) | ConvertFrom-Json
$photos = New-Object System.Collections.Generic.List[object]
foreach ($name in @($Fixture | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    $entry = @($fixtureManifest.fixtures | Where-Object { $_.id -eq $name }) | Select-Object -First 1
    if ($entry) {
        $path = Join-Path $repoRoot ("fixtures\cache\{0}\{1}" -f $entry.id, $entry.file_name)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "The fixture $name is not cached: run tools\fetch-fixtures.ps1." }
        $photos.Add([pscustomobject]@{ Folder = $entry.id; Path = $path })
    }
    elseif (Test-Path -LiteralPath $name -PathType Leaf) {
        $full = (Resolve-Path -LiteralPath $name).Path
        $photos.Add([pscustomobject]@{ Folder = ('photo-{0}' -f ($photos.Count + 1)); Path = $full })
    }
    else { throw "'$name' is neither a fixture id of fixtures\manifest.json nor a file." }
}
if ($photos.Count -eq 0) { throw 'Give at least one photo (-Fixture).' }

if (-not $WorkDir) { $WorkDir = 'build\clean-machine-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$WorkDir = Resolve-RepoPath $WorkDir
if (Test-Path -LiteralPath $WorkDir) { throw "$WorkDir already exists: every gate run needs a new folder." }
$inputDir = Join-Path $WorkDir 'input'
$resultsDir = Join-Path $WorkDir 'results'
foreach ($folder in $inputDir, $resultsDir, (Join-Path $inputDir 'scripts'), (Join-Path $inputDir 'fixtures')) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
Write-Host "Gate folder: $WorkDir"
Copy-Item -LiteralPath $installer.FullName -Destination $inputDir
Copy-Item -LiteralPath $manifest -Destination $inputDir
if ($rehearsal.Count -gt 0) {
    New-Item -ItemType Directory -Path (Join-Path $inputDir 'rehearsal') | Out-Null
    Copy-Item -LiteralPath $rehearsal[0].FullName -Destination (Join-Path $inputDir 'rehearsal')
}
foreach ($photo in $photos) {
    $target = Join-Path $inputDir ("fixtures\{0}" -f $photo.Folder)
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath $photo.Path -Destination $target
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'installer\sandbox\Invoke-CleanMachineGate.ps1') -Destination (Join-Path $inputDir 'scripts')
Copy-Item -LiteralPath (Join-Path $repoRoot 'installer\tests\Test-InstalledPackage.ps1') -Destination (Join-Path $inputDir 'scripts')
$request = [ordered]@{ gate = 'clean-machine'; mode = $(if ($LocalRehearsal) { 'rehearsal' } elseif ($Manual) { 'manual' } else { 'sandbox' })
    prepared = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); installer = $installer.Name; installerSha256 = $installerSha256
    versionRehearsal = $(if ($rehearsal.Count) { $rehearsal[0].Name } else { $null }); photos = $photos.Count
    fixtures = @($photos | ForEach-Object { Split-Path -Leaf $_.Path }); skipInstallerTest = [bool]$SkipInstallerTest
    host = (Get-LucentSandboxHostFacts) }
[System.IO.File]::WriteAllText((Join-Path $WorkDir 'gate-request.json'), (ConvertTo-Json -InputObject $request -Depth 4), $utf8)
Write-Host ("  {0}{1}; {2} photo(s): {3}" -f $installer.Name, $(if ($rehearsal.Count) { " and its version rehearsal $($rehearsal[0].Name)" } else { '' }),
    $photos.Count, ($request.fixtures -join ', '))

# --- 2. The sandbox configuration --------------------------------------------------------------------

$gateArguments = @()
if (-not $KeepOpen) { $gateArguments += '-Shutdown' }
if ($SkipInstallerTest) { $gateArguments += '-SkipInstallerTest' }
$wsb = Join-Path $WorkDir 'lucent-clean-machine.wsb'
[System.IO.File]::WriteAllText($wsb, (New-LucentSandboxConfiguration -InputDir $inputDir -ResultsDir $resultsDir -MemoryMB $MemoryMB -GateArguments $gateArguments -Manual:$Manual), $utf8)
$validation = Test-LucentSandboxConfiguration -Path $wsb -InputDir $inputDir -ResultsDir $resultsDir -Manual:$Manual
if (-not $validation.Valid) {
    foreach ($problem in $validation.Problems) { Write-Host "  ERROR: $problem" -ForegroundColor Red }
    throw "$wsb is not a valid gate configuration."
}
Write-Host "Windows Sandbox configuration: $wsb (valid against installer\sandbox\wsb-configuration.xsd; networking off, input read-only, results writable)"

# --- 3. Run ------------------------------------------------------------------------------------------

if ($NoStart) {
    Write-Host "Prepared. Start it with  Start-Process '$wsb'  and when results\gate.json exists check it with  tools\Test-CleanMachine.ps1 -Check '$WorkDir'"
    exit 0
}
if ($LocalRehearsal) {
    $profileDir = Join-Path $WorkDir 'profile'
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $inputDir 'scripts\Invoke-CleanMachineGate.ps1'),
        '-Root', $WorkDir, '-Rehearsal', '-InstallDir', (Join-Path $profileDir 'Programs\Lucent'))
    if ($SkipInstallerTest) { $arguments += '-SkipInstallerTest' }
    Write-Host 'Local rehearsal: the gate''s logon script runs here (installs for this user into the gate folder, then uninstalls).'
    & powershell.exe @arguments
    Complete-Gate $WorkDir
}

$sandbox = Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe'
if (-not (Test-Path -LiteralPath $sandbox -PathType Leaf)) {
    Write-Host "Windows Sandbox is not enabled on this computer ($sandbox is missing). An administrator enables it once and restarts Windows: see docs\release\clean-machine.md. The gate folder is ready: $WorkDir" -ForegroundColor Yellow
    exit 2
}
# One sandbox at a time: a second start is refused, and its gate would never run (W12 review CMG-02).
$running = @(Get-LucentSandboxInstances)
if ($running.Count -gt 0) {
    $verdict = Write-HostCheck -Folder $WorkDir -Passed $false -Outcome 'not started: another Windows Sandbox is running' `
        -Problems @("Windows Sandbox runs one instance at a time, and one is running: $($running -join ', ')")
    Write-Host "Windows Sandbox is already running ($($running -join ', ')); it runs one instance at a time. Close that sandbox (everything in it is discarded), then start this gate folder with  Start-Process '$wsb'  and check it with  tools\Test-CleanMachine.ps1 -Check '$WorkDir'  (or prepare a new one). The host's verdict: $verdict" -ForegroundColor Red
    exit 1
}
if ($Manual) {
    Start-Process -FilePath $sandbox -ArgumentList "`"$wsb`""
    Write-Host 'Windows Sandbox started with Explorer at C:\LucentGate\input: follow the manual steps of docs\release\clean-machine.md.'
    exit 0
}
Write-Host 'Starting Windows Sandbox. It runs the gate elevated, so no elevation prompt is expected (should one appear in the sandbox window, answer Yes); gate.json records the gate''s token and UAC settings.'
Start-Process -FilePath $sandbox -ArgumentList "`"$wsb`""
$startedAt = Get-Date
$deadline = $startedAt.AddMinutes($TimeoutMinutes)
$startDeadline = $startedAt.AddMinutes($StartTimeoutMinutes)
$gatePath = Join-Path $resultsDir 'gate.json'
$progressPath = Join-Path $resultsDir 'progress.json'
$lastStep = ''
$gateStarted = $false
while (-not (Test-Path -LiteralPath $gatePath -PathType Leaf)) {
    if (-not $gateStarted -and (Test-Path -LiteralPath $progressPath -PathType Leaf)) { $gateStarted = $true }
    if (-not $gateStarted -and (Get-Date) -gt $startDeadline) {
        $diagnosis = @(Get-StartDiagnosis)
        $verdict = Write-HostCheck -Folder $WorkDir -Passed $false -Outcome 'the gate never started' `
            -Problems (@("The gate wrote no progress within $StartTimeoutMinutes minutes of starting Windows Sandbox: its logon command did not run.") + $diagnosis)
        Write-Host "The gate has not started within $StartTimeoutMinutes minutes ($progressPath does not exist): the sandbox did not run its logon command." -ForegroundColor Red
        foreach ($line in $diagnosis) { Write-Host "  $line" -ForegroundColor Red }
        Write-Host "Close the sandbox window if one is open. The host's verdict: $verdict" -ForegroundColor Red
        exit 1
    }
    if ((Get-Date) -gt $deadline) {
        $verdict = Write-HostCheck -Folder $WorkDir -Passed $false -Outcome "timed out at step '$lastStep'" `
            -Problems @("No result after $TimeoutMinutes minutes; the gate's last step was '$lastStep'.")
        Write-Host "No result after $TimeoutMinutes minutes ($gatePath); the gate's last step was '$lastStep'. The sandbox window may still be open: look at it, then close it. The host's verdict: $verdict" -ForegroundColor Red
        exit 1
    }
    Start-Sleep -Seconds 10
    if (Test-Path -LiteralPath $progressPath -PathType Leaf) {
        $gateStarted = $true
        $step = $lastStep
        try { $step = [string](Read-CleanMachineJson -Path $progressPath -TimeoutSeconds 0).step } catch { $step = $lastStep }
        if ($step -and $step -ne $lastStep) { Write-Host ("  {0:HH:mm:ss} gate: {1}" -f (Get-Date), $step); $lastStep = $step }
    }
}
# The gate writes gate.json complete under another name and renames it, and Complete-Gate reads it with
# retries (SBX-2). With -Shutdown the sandbox closes about 15 seconds later: wait for that, so the next
# run finds no sandbox running.
if (-not $KeepOpen) {
    $closeDeadline = (Get-Date).AddMinutes(3)
    while (@(Get-LucentSandboxInstances).Count -gt 0 -and (Get-Date) -lt $closeDeadline) { Start-Sleep -Seconds 5 }
    if (@(Get-LucentSandboxInstances).Count -gt 0) { Write-Host '  NOTE: the sandbox has not closed 3 minutes after the gate wrote its result; close it yourself.' -ForegroundColor Yellow }
}
Complete-Gate $WorkDir
