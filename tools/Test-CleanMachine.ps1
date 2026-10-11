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
       asks for (the photos, the installer's SHA-256, the mode, the installer test).
    2. Writes <WorkDir>\lucent-clean-machine.wsb (installer\sandbox\LucentSandbox.psm1): networking,
       the virtual GPU, the clipboard, printers, audio and video input off; input\ read-only at
       C:\LucentGate\input, results\ writable at C:\LucentGate\results; at logon the gate. Validates it
       against installer\sandbox\wsb-configuration.xsd (Microsoft's documented elements and values) and
       the gate's rules.
    3. Starts Windows Sandbox with it, and waits at most -TimeoutMinutes for results\gate.json,
       showing the gate's step (results\progress.json). On a clean Windows, Setup installs Microsoft's
       Visual C++ runtime first, which asks for elevation once: answer Yes in the sandbox window. The
       gate then shuts the sandbox down (unless -KeepOpen).
    4. Checks gate.json against gate-request.json (Test-CleanMachineResult): every check of the gate
       present and passed, the machine clean and offline, one JPEG and one 16-bit TIFF per photo, the
       package's Setup. Writes results\host-check.json. The gate's own verdict is not enough.

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
    Exit code 0: the gate passed (or was prepared or started as asked); 1: it failed or timed out;
    2: Windows Sandbox is not available. Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [string]$PackageDir = 'build\release\package',
    [string[]]$Fixture = @('dji-fc4382-dng', 'fujifilm-x-t4-raf-lossless', 'nikon-z-6-nef-lossless'),
    [string]$WorkDir,
    [ValidateRange(5, 600)][int]$TimeoutMinutes = 60,
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

# 4. The result of a gate folder, checked against what was asked for rather than taken from the gate's own verdict.
function Complete-Gate([string]$Folder) {
    $request = [System.IO.File]::ReadAllText((Join-Path $Folder 'gate-request.json')) | ConvertFrom-Json
    $resultsDir = Join-Path $Folder 'results'
    $gatePath = Join-Path $resultsDir 'gate.json'
    if (-not (Test-Path -LiteralPath $gatePath -PathType Leaf)) { Write-Host "No result: $gatePath is missing." -ForegroundColor Red; exit 1 }
    $gate = [System.IO.File]::ReadAllText($gatePath) | ConvertFrom-Json
    $rehearsed = $request.mode -eq 'rehearsal'
    $result = Test-CleanMachineResult -Gate $gate -Photos ([int]$request.photos) -InstallerSha256 $request.installerSha256 -Rehearsal:$rehearsed `
        -SkipInstallerTest:([bool]$request.skipInstallerTest)
    $record = [ordered]@{ checkedAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); passed = $result.Passed; problems = @($result.Problems)
        request = $request; host = [ordered]@{ windows = [System.Environment]::OSVersion.VersionString; computer = $env:COMPUTERNAME } }
    [System.IO.File]::WriteAllText((Join-Path $resultsDir 'host-check.json'), (ConvertTo-Json -InputObject $record -Depth 6), $utf8)
    foreach ($problem in $result.Problems) { Write-Host "  PROBLEM: $problem" -ForegroundColor Red }
    $environment = if ($gate.PSObject.Properties.Name -contains 'environment') { $gate.environment } else { $null }
    Write-Host ("Clean-machine gate{0}: {1}. {2} of {3} checks passed; machine clean: {4}, offline: {5}. Results: {6}" -f
        $(if ($rehearsed) { ' (local rehearsal: not the gate itself)' } else { '' }), $(if ($result.Passed) { 'PASSED' } else { 'FAILED' }),
        @($gate.checks | Where-Object { $_.passed }).Count, @($gate.checks).Count,
        $(if ($environment) { $environment.clean } else { 'unknown' }), $(if ($environment) { $environment.network.offline } else { 'unknown' }), $resultsDir) `
        -ForegroundColor $(if ($result.Passed) { 'Green' } else { 'Red' })
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
    fixtures = @($photos | ForEach-Object { Split-Path -Leaf $_.Path }); skipInstallerTest = [bool]$SkipInstallerTest }
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
if ($Manual) {
    Start-Process -FilePath $sandbox -ArgumentList "`"$wsb`""
    Write-Host 'Windows Sandbox started with Explorer at C:\LucentGate\input: follow the manual steps of docs\release\clean-machine.md.'
    exit 0
}
Write-Host 'Starting Windows Sandbox. On a clean Windows, Setup installs the Visual C++ runtime first: answer Yes when Windows asks for permission inside the sandbox.'
Start-Process -FilePath $sandbox -ArgumentList "`"$wsb`""
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$gatePath = Join-Path $resultsDir 'gate.json'
$progressPath = Join-Path $resultsDir 'progress.json'
$lastStep = ''
while (-not (Test-Path -LiteralPath $gatePath -PathType Leaf)) {
    if ((Get-Date) -gt $deadline) {
        Write-Host "No result after $TimeoutMinutes minutes ($gatePath). The sandbox window may still be open: look at it, then close it." -ForegroundColor Red
        exit 1
    }
    Start-Sleep -Seconds 10
    if (Test-Path -LiteralPath $progressPath -PathType Leaf) {
        try { $step = ([System.IO.File]::ReadAllText($progressPath) | ConvertFrom-Json).step } catch { $step = $lastStep }
        if ($step -and $step -ne $lastStep) { Write-Host ("  {0:HH:mm:ss} gate: {1}" -f (Get-Date), $step); $lastStep = $step }
    }
}
Start-Sleep -Seconds 2   # the gate writes gate.json last; let the file system settle
Complete-Gate $WorkDir
