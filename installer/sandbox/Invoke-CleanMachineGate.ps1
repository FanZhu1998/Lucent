<#
.SYNOPSIS
    The clean-machine gate of a Lucent package (blueprint section 10, "Offline installation"), run at
    logon inside Windows Sandbox: installs Setup silently, imports, edits (Auto) and exports JPEG and
    16-bit TIFF through the installed Lucent.exe, uninstalls, and writes gate.json
    (tools\Test-CleanMachine.ps1; docs\release\clean-machine.md).

.DESCRIPTION
    -Root (C:\LucentGate in the sandbox) holds what tools\Test-CleanMachine.ps1 prepared:
      input\     read-only: Lucent-Setup-<version>-x64.exe and its .manifest.json, rehearsal\ (the
                 package's version rehearsal Setup, when it has one), fixtures\ (a few RAW files),
                 scripts\ (this script and installer\tests\Test-InstalledPackage.ps1)
      results\   writable, on the host: what the gate records
    and the gate works in <Root>\work (the sandbox's own disk).

    1. The machine (environment.json): Windows version; whether the Visual C++ runtime is registered
       (Microsoft's detection key, both registry views) or its DLLs are in System32; developer
       programs on PATH (python, py, git, cl, link, cmake, ninja, vcpkg, qmake, qmake6, windeployqt);
       Qt variables; whether a network is available and a host name resolves; and the gate's own
       token and Windows' UAC settings (elevation: Administrators role, integrity level, elevation
       type, EnableLUA, ConsentPromptBehaviorAdmin and -User, PromptOnSecureDesktop,
       FilterAdministratorToken), which decide whether the runtime installer's elevation can show a
       consent prompt at all (W12 review CMG-01). Clean: no runtime, no such program or variable.
       Offline: no network.
    2. Setup /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER (and /DIR=-InstallDir): exit code
       0, Lucent.exe where the uninstall registration says, every installed file as the package
       manifest records it (SHA-256). On a clean machine Setup installs Microsoft's Visual C++
       runtime first (ShellExec 'runas'). Windows Sandbox runs this logon script with an elevated
       administrator token, so the runtime installs without a consent prompt; install.exit names
       Setup's "User privileges" line, and gate.json's notes say that the consent path was not
       exercised (the manual steps of docs\release\clean-machine.md cover it). Should a future
       Windows show a prompt in the sandbox window, answer Yes.
    3. The installed Lucent.exe with PATH = %SystemRoot%\System32;%SystemRoot%, no Qt variables and a
       throwaway --data-root, through its developer automation (apps\lucent\src\developer_automation.hpp):
         run 1: --import <fixtures> --include-subfolders --open-first --auto --export-all <work>\jpeg
                --export-format jpeg --screenshot results\lucent-auto-jpeg.png --quit-after-screenshot
         run 2: --export-all <work>\tiff --export-format tiff16 --screenshot results\lucent-tiff.png
                --quit-after-screenshot (the library run 1 left in the data root)
       Each must exit with 0 and save its screenshot, without Qt or QML loading errors, report the
       installed camera support and lens databases as active (from the installation folder), and
       export every photo, one file per photo: a JPEG (SOI and EOI markers), and a 16-bit RGB TIFF
       (its first image file directory read: BitsPerSample 16, 16, 16, SamplesPerPixel 3, chunky
       planar configuration); run 1 must finish Auto on the first photo with a summary. The fixtures
       are unchanged afterwards (SHA-256). Lucent.exe's output is read as plain text lines
       (Read-TextLines): a line of Get-Content carries PowerShell's provider properties, which
       ConvertTo-Json writes out in full (W12 sandbox run SBX-1: a 109 MB gate.json).
    4. The uninstaller, silently: the program folder, the shortcuts and the registration are gone,
       the data root is kept.
    5. Unless -SkipInstallerTest: installer\tests\Test-InstalledPackage.ps1 -RuntimeOutcomes (with
       -NewerInstaller when input\rehearsal holds the version rehearsal: install, same-version repair,
       upgrade and a refused downgrade) in <Root>\work\installer-test; its result.json and logs are
       copied to results\installer-test.
    6. results\gate.json: every check with a stable id, the environment, the runs' outputs (always
       arrays), notes (what the run could not show, never failing); passed when every check passed (in
       a -Rehearsal the environment's checks are recorded, not required). Every result file is written
       complete under a temporary name (<name>.partial) and then renamed, so the host never reads a
       half-written gate.json (W12 sandbox run SBX-2); a result larger than 1 MiB is refused (a
       failed gate.json says why). results\progress.json says which step runs. With -Shutdown,
       Windows shuts down 15 seconds later, which closes the sandbox.

    -Rehearsal runs the same steps on any computer (tools\Test-CleanMachine.ps1 -LocalRehearsal): Lucent
    must not be installed for the user, Setup installs into -InstallDir (a temporary profile-like
    folder), and the machine is usually not clean.

.PARAMETER Root
    The gate's folder (default C:\LucentGate).
.PARAMETER Rehearsal
    Not in Windows Sandbox (see DESCRIPTION).
.PARAMETER InstallDir
    Setup's /DIR (default: its own, %LOCALAPPDATA%\Programs\Lucent).
.PARAMETER SettleTimeoutMs
    Lucent.exe --settle-timeout of each run (default 900000: imports, Auto and exports included).
.PARAMETER SkipInstallerTest
    Leave out step 5.
.PARAMETER Shutdown
    Shut Windows down when done (closes the sandbox).

.NOTES
    Needs no developer tools: Windows PowerShell 5.1 as Windows ships it. Keep this file ASCII.
    installer\tests\Test-CleanMachineHarness.ps1 tests the helper functions (ConvertTo-GateJsonText,
    Read-TextLines, Get-TiffLayout, Get-GateElevation) on their own: keep them free of the script's
    variables.
#>
[CmdletBinding()]
param(
    [string]$Root = 'C:\LucentGate',
    [switch]$Rehearsal,
    [string]$InstallDir,
    [ValidateRange(60000, 7200000)][int]$SettleTimeoutMs = 900000,
    [switch]$SkipInstallerTest,
    [switch]$Shutdown
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$started = Get-Date
$inputDir = Join-Path $Root 'input'
$results = Join-Path $Root 'results'
$work = Join-Path $Root 'work'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$appGuid = '{ADBA4913-BBBD-42D8-90EF-0E9C80412232}'   # AppId in installer\lucent.iss
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($appGuid)_is1"

# --- Helpers (tested on their own by installer\tests\Test-CleanMachineHarness.ps1) -------------------

function ConvertTo-GateJsonText($Value, [int]$Limit = 1048576) {
    # The JSON of a result. A gate result is a few kilobytes; one beyond -Limit characters holds
    # something that is not plain data (an object PowerShell serialises with its type information), so
    # it is refused instead of written (W12 sandbox run SBX-1).
    $text = ConvertTo-Json -InputObject $Value -Depth 8
    if ($text.Length -gt $Limit) { throw "The result would be $($text.Length) characters of JSON (more than $Limit): it holds data that is not plain text and numbers." }
    return $text
}
function Read-TextLines([string]$Path) {
    # The lines of a UTF-8 text file as plain strings (Get-Content decorates each line with provider
    # properties that ConvertTo-Json writes out); none when the file is missing.
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return ,([string[]]@()) }
    return ,([System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8))
}
function Get-TiffLayout([byte[]]$Bytes) {
    # The first image file directory of a classic TIFF (TIFF 6.0): byte order, BitsPerSample (tag
    # 258), SamplesPerPixel (277) and PlanarConfiguration (284, 1 when absent). Problem says why the
    # bytes are not such a file (BigTIFF included); the other fields are then not set.
    $layout = [ordered]@{ ByteOrder = $null; BitsPerSample = @(); SamplesPerPixel = $null; PlanarConfiguration = 1; Problem = $null }
    if ($null -eq $Bytes -or $Bytes.Length -lt 8) { $layout.Problem = 'shorter than a TIFF header'; return $layout }
    if ($Bytes[0] -eq 0x49 -and $Bytes[1] -eq 0x49) { $little = $true } elseif ($Bytes[0] -eq 0x4D -and $Bytes[1] -eq 0x4D) { $little = $false }
    else { $layout.Problem = 'no TIFF byte order mark'; return $layout }
    $read16 = { param([int64]$At) if ($little) { [int]$Bytes[$At] + 256 * [int]$Bytes[$At + 1] } else { 256 * [int]$Bytes[$At] + [int]$Bytes[$At + 1] } }
    $read32 = { param([int64]$At) if ($little) { [int64](& $read16 $At) + 65536 * [int64](& $read16 ($At + 2)) } else { 65536 * [int64](& $read16 $At) + [int64](& $read16 ($At + 2)) } }
    $layout.ByteOrder = if ($little) { 'II' } else { 'MM' }
    if ((& $read16 2) -ne 42) { $layout.Problem = "version $(& $read16 2), not 42 (a classic TIFF)"; return $layout }
    $ifd = & $read32 4
    if ($ifd -lt 8 -or $ifd + 2 -gt $Bytes.Length) { $layout.Problem = "its first directory offset $ifd lies outside the file"; return $layout }
    $count = & $read16 $ifd
    if ($ifd + 2 + 12 * $count + 4 -gt $Bytes.Length) { $layout.Problem = "its first directory ($count entries) runs past the end of the file"; return $layout }
    for ($i = 0; $i -lt $count; $i++) {
        $entry = $ifd + 2 + 12 * $i
        $tag = & $read16 $entry
        if ($tag -notin 258, 277, 284) { continue }
        $type = & $read16 ($entry + 2)
        $values = & $read32 ($entry + 4)
        if ($type -ne 3) { $layout.Problem = "tag $tag has type $type, not SHORT"; return $layout }
        # SHORT values: in the entry when they fit in 4 bytes, otherwise at the offset it holds.
        $at = if ($values * 2 -le 4) { $entry + 8 } else { & $read32 ($entry + 8) }
        if ($values -lt 1 -or $values -gt 64 -or $at + 2 * $values -gt $Bytes.Length) { $layout.Problem = "tag $tag's $values values lie outside the file"; return $layout }
        $read = @(for ($k = 0; $k -lt $values; $k++) { & $read16 ($at + 2 * $k) })
        switch ($tag) {
            258 { $layout.BitsPerSample = $read }
            277 { $layout.SamplesPerPixel = $read[0] }
            284 { $layout.PlanarConfiguration = $read[0] }
        }
    }
    return $layout
}
function Get-GateElevation {
    # The gate's token and Windows' UAC settings (W12 review CMG-01): whether Setup's elevated runtime
    # installer (ShellExec 'runas') can show a consent or credential prompt here at all. Runtime says
    # what to expect and why.
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $administrator = (New-Object System.Security.Principal.WindowsPrincipal($identity)).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
    $integritySid = $null
    try {
        foreach ($line in @(& (Join-Path $env:SystemRoot 'System32\whoami.exe') /groups /fo csv /nh)) {
            if ([string]$line -match '"(S-1-16-\d+)"') { $integritySid = $Matches[1] }
        }
    }
    catch { $integritySid = $null }
    $levels = @{ 'S-1-16-0' = 'untrusted'; 'S-1-16-4096' = 'low'; 'S-1-16-8192' = 'medium'; 'S-1-16-8448' = 'medium plus'; 'S-1-16-12288' = 'high'; 'S-1-16-16384' = 'system' }
    $integrity = if ($integritySid -and $levels.ContainsKey($integritySid)) { $levels[$integritySid] } else { $integritySid }
    $elevationType = $null
    try {
        if (-not ('LucentGate.Token' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LucentGate {
    public static class Token {
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returned);
        // TOKEN_ELEVATION_TYPE (TokenElevationType, 18): 1 default (no split token: UAC off, or not an
        // administrator), 2 full (elevated), 3 limited (an administrator's filtered token); 0 unknown.
        public static int ElevationType(IntPtr token) {
            int type, returned;
            return GetTokenInformation(token, 18, out type, 4, out returned) ? type : 0;
        }
    }
}
'@
        }
        $elevationType = switch ([LucentGate.Token]::ElevationType($identity.Token)) { 1 { 'default' } 2 { 'full' } 3 { 'limited' } default { $null } }
    }
    catch { $elevationType = $null }
    $uac = [ordered]@{}
    foreach ($name in 'EnableLUA', 'ConsentPromptBehaviorAdmin', 'ConsentPromptBehaviorUser', 'PromptOnSecureDesktop', 'FilterAdministratorToken') {
        $value = $null
        try { $value = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name $name -ErrorAction Stop).$name } catch { $value = $null }
        $uac[$name] = $(if ($null -eq $value) { $null } else { [int]$value })
    }
    $prompt = $null
    $why = 'the token or the UAC settings could not be read'
    if ($elevationType -eq 'full' -or $integrity -in 'high', 'system') { $prompt = $false; $why = 'the gate runs elevated: Setup and the runtime installer inherit its administrator token' }
    elseif ($uac['EnableLUA'] -eq 0) { $prompt = $false; $why = 'UAC is off (EnableLUA 0)' }
    elseif ($elevationType -eq 'limited') {
        if ($uac['ConsentPromptBehaviorAdmin'] -eq 0) { $prompt = $false; $why = 'an administrator''s elevation needs no consent here (ConsentPromptBehaviorAdmin 0)' }
        else { $prompt = $true; $why = 'an administrator''s filtered token: Windows asks for consent' }
    }
    elseif ($elevationType -eq 'default' -and -not $administrator) {
        $prompt = $true; $why = 'a standard user: Windows asks for an administrator''s credentials'
    }
    return [ordered]@{
        administratorsRole = $administrator; integrityLevel = $integrity; elevationType = $elevationType; uac = $uac
        runtimePromptExpected = $prompt; runtimePrompt = $why }
}

function Write-Json([string]$Name, $Value) {
    # Complete under a temporary name, then renamed: the host waits for the final name (SBX-2).
    $text = ConvertTo-GateJsonText $Value
    $path = Join-Path $results $Name
    $partial = "$path.partial"
    [System.IO.File]::WriteAllText($partial, $text, $utf8)
    try {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        [System.IO.File]::Move($partial, $path)
    }
    catch {
        [System.IO.File]::WriteAllText($path, $text, $utf8)
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    }
}
function Set-GateProgress([string]$Step) {
    Write-Host "== $Step" -ForegroundColor Cyan
    Write-Json 'progress.json' ([ordered]@{ step = $Step; at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') })
}
$checks = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
function Add-Check([string]$Id, [string]$Name, [bool]$Passed, [string]$Detail = '') {
    $checks.Add([ordered]@{ id = $Id; name = $Name; passed = $Passed; detail = $Detail })
    Write-Host ("[{0}] {1}{2}" -f $(if ($Passed) { 'PASS' } else { 'FAIL' }), $Name, $(if ($Detail) { " - $Detail" } else { '' })) `
        -ForegroundColor $(if ($Passed) { 'Green' } else { 'Red' })
}
function Add-Note([string]$Text) {
    $notes.Add($Text)
    Write-Host "NOTE: $Text" -ForegroundColor Yellow
}
function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
# Runs the installed uninstaller silently and waits until it has removed the registration and itself
# (it continues in a copy of itself in %TEMP%, whose folder it leaves for its next run: removed here).
$script:appDir = ''
function Invoke-GateUninstall {
    if (-not $script:appDir) { return }
    $uninstaller = Join-Path $script:appDir 'unins000.exe'
    if (-not (Test-Path -LiteralPath $uninstaller)) { return }
    $temporary = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'is-*-uninstall.tmp' -ErrorAction SilentlyContinue | ForEach-Object FullName)
    Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$(Join-Path $results 'uninstall.log')`"") -Wait | Out-Null
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline -and ((Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath $uninstaller))) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 2
    foreach ($folder in @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'is-*-uninstall.tmp' -ErrorAction SilentlyContinue | ForEach-Object FullName | Where-Object { $temporary -notcontains $_ })) {
        if (Test-Path -LiteralPath (Join-Path $folder '_unins-done.tmp')) { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

if (-not (Test-Path -LiteralPath $results -PathType Container)) { New-Item -ItemType Directory -Path $results | Out-Null }
if (Test-Path -LiteralPath (Join-Path $results 'gate.json')) { throw "$results already holds a gate.json: every gate run needs a new results folder." }
if (Test-Path -LiteralPath $work) { throw "$work exists: this machine already ran the gate. Start a new sandbox (or a new -Root)." }
New-Item -ItemType Directory -Path $work | Out-Null
$transcript = Join-Path $results 'gate-transcript.txt'
try { Start-Transcript -LiteralPath $transcript -Force | Out-Null } catch { }

$outcome = [ordered]@{ gate = 'clean-machine'; format = 1; mode = $(if ($Rehearsal) { 'rehearsal' } else { 'sandbox' }); passed = $false }
try {
    # --- 1. The machine -------------------------------------------------------------------------------
    Set-GateProgress 'machine'
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    $runtime = foreach ($view in 'Registry64', 'Registry32') {
        $values = $null
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::$view)
            $key = $base.OpenSubKey('SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64')
            if ($key) { $values = [ordered]@{ installed = $key.GetValue('Installed'); version = $key.GetValue('Version') }; $key.Dispose() }
            $base.Dispose()
        }
        catch { $values = $null }
        [ordered]@{ view = $view; registration = $values }
    }
    $runtimeRegistered = @($runtime | Where-Object { $_.registration -and $_.registration.installed -eq 1 }).Count -gt 0
    $runtimeDlls = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll' | Where-Object { Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$_") -PathType Leaf })
    $tools = @('python', 'python3', 'py', 'git', 'cl', 'link', 'cmake', 'ninja', 'vcpkg', 'qmake', 'qmake6', 'windeployqt' | ForEach-Object {
        $found = Get-Command -Name "$_.exe" -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { [ordered]@{ name = $_; path = [string]$found.Source } } })
    $qtVariables = @(Get-ChildItem Env: | Where-Object { $_.Name -match '^(QT_|QML2?_)' } | ForEach-Object { [string]$_.Name })
    $networkAvailable = [System.Net.NetworkInformation.NetworkInterface]::GetIsNetworkAvailable()
    $resolves = $false
    try { $resolves = @([System.Net.Dns]::GetHostAddresses('www.microsoft.com')).Count -gt 0 } catch { $resolves = $false }
    $offline = (-not $networkAvailable) -and (-not $resolves)
    $clean = (-not $runtimeRegistered) -and @($tools).Count -eq 0 -and $qtVariables.Count -eq 0
    $elevation = Get-GateElevation
    $environment = [ordered]@{
        recorded = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); computer = [string]$env:COMPUTERNAME; user = [string]$env:USERNAME
        windows = [ordered]@{ caption = [string]$os.Caption; version = [string]$os.Version; build = [string]$os.BuildNumber }
        vcRuntime = [ordered]@{ registered = $runtimeRegistered; registrations = @($runtime); dllsInSystem32 = $runtimeDlls }
        developerPrograms = @($tools); qtVariables = $qtVariables
        network = [ordered]@{ available = $networkAvailable; resolvesHostNames = $resolves; offline = $offline }
        elevation = $elevation
        clean = $clean }
    Write-Json 'environment.json' $environment
    $outcome['environment'] = $environment
    Add-Check 'environment.clean' 'The machine is clean: no Visual C++ runtime, developer program or Qt variable' $clean `
        ("runtime {0}; programs: {1}; Qt variables: {2}" -f $(if ($runtimeRegistered) { 'registered' } else { 'absent' }),
            $(if (@($tools).Count) { (@($tools) | ForEach-Object { $_.name }) -join ', ' } else { 'none' }), $(if ($qtVariables.Count) { $qtVariables -join ', ' } else { 'none' }))
    Add-Check 'environment.offline' 'The machine is offline: no network, no name resolves' $offline ("network available: $networkAvailable; resolves: $resolves")
    Write-Host ("The gate's token: Administrators role {0}, integrity {1}, elevation type {2}; UAC: EnableLUA {3}, ConsentPromptBehaviorAdmin {4}. The runtime installer's elevation: {5}." -f
        $elevation.administratorsRole, $elevation.integrityLevel, $elevation.elevationType, $elevation.uac['EnableLUA'], $elevation.uac['ConsentPromptBehaviorAdmin'], $elevation.runtimePrompt)

    # --- The package and the fixtures ----------------------------------------------------------------
    $installers = @(Get-ChildItem -LiteralPath $inputDir -File -Filter 'Lucent-Setup-*-x64.exe' | Where-Object { $_.Name -notmatch '-TEST-SIGNED|-VERSION-REHEARSAL' })
    if ($installers.Count -ne 1) { throw "$inputDir holds $($installers.Count) Lucent installers; expected one." }
    $installer = $installers[0].FullName
    $manifestPath = Join-Path $inputDir ($installers[0].BaseName + '.manifest.json')
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "The package manifest $manifestPath is missing." }
    $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $rehearsalSetup = @(Get-ChildItem -LiteralPath (Join-Path $inputDir 'rehearsal') -File -Filter 'Lucent-Setup-*-x64-VERSION-REHEARSAL.exe' -ErrorAction SilentlyContinue | Select-Object -First 1)
    $fixtures = @(Get-ChildItem -LiteralPath (Join-Path $inputDir 'fixtures') -File -Recurse | Sort-Object FullName)
    if ($fixtures.Count -eq 0) { throw "$inputDir\fixtures holds no photo." }
    $fixtureHashes = @{}
    foreach ($fixture in $fixtures) { $fixtureHashes[$fixture.FullName] = Get-Sha256 $fixture.FullName }
    $outcome['installer'] = [ordered]@{ file = [string]$installers[0].Name; sha256 = (Get-Sha256 $installer); version = [string]$manifest.version; gitCommit = [string]$manifest.gitCommit }
    $outcome['fixtures'] = @($fixtures | ForEach-Object { [ordered]@{ file = [string]$_.FullName.Substring($inputDir.Length + 1); bytes = $_.Length; sha256 = $fixtureHashes[$_.FullName] } })
    if (Test-Path -LiteralPath $uninstallKey) {
        throw "Lucent is already installed for this user ($((Get-ItemProperty -LiteralPath $uninstallKey).InstallLocation)); the gate never touches a real installation."
    }

    # --- 2. Install -----------------------------------------------------------------------------------
    Set-GateProgress 'install'
    $installLog = Join-Path $results 'install.log'
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/LOG=`"$installLog`"")
    if ($InstallDir) { $arguments += "/DIR=`"$InstallDir`"" }
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $setupProcess = Start-Process -FilePath $installer -ArgumentList $arguments -PassThru -Wait
    $installLines = Read-TextLines $installLog
    $runtimeLines = @($installLines | Where-Object { $_ -match 'VC\+\+ runtime:' })
    $privileges = @($installLines | Where-Object { $_ -match 'User privileges: ' } | ForEach-Object { ($_ -replace '^.*(User privileges: )', '$1').Trim() }) | Select-Object -First 1
    Add-Check 'install.exit' 'Setup installed silently (exit code 0)' ($setupProcess.ExitCode -eq 0) `
        ("exit code {0}, {1:N0} s; Setup's {2}; {3}" -f $setupProcess.ExitCode, $timer.Elapsed.TotalSeconds, $(if ($privileges) { $privileges } else { 'user privileges not logged' }), ($runtimeLines -join ' | '))
    if (@($runtimeLines | Where-Object { $_ -match 'running vc_redist' }).Count -gt 0 -and $elevation.runtimePromptExpected -eq $false) {
        Add-Note ("Setup installed the Visual C++ runtime without a consent prompt ($($elevation.runtimePrompt); Setup's $(if ($privileges) { $privileges } else { 'privileges unknown' })): " +
            'this run does not exercise the consent path (a prompt accepted or declined); the manual steps of docs\release\clean-machine.md do.')
    }
    $registration = if (Test-Path -LiteralPath $uninstallKey) { Get-ItemProperty -LiteralPath $uninstallKey } else { $null }
    $appDir = if ($registration) { ([string]$registration.InstallLocation).TrimEnd('\') } else { '' }
    $script:appDir = $appDir
    $exe = if ($appDir) { Join-Path $appDir 'Lucent.exe' } else { '' }
    Add-Check 'install.registration' 'Registered for this user, Lucent.exe where the registration says' ([bool]$registration -and (Test-Path -LiteralPath $exe)) `
        $(if ($registration) { "$($registration.DisplayName) $($registration.DisplayVersion) in $appDir" } else { 'no registration' })
    if (-not $registration -or -not (Test-Path -LiteralPath $exe)) { throw 'Setup did not install Lucent; the gate cannot continue.' }
    $mismatch = @()
    foreach ($file in $manifest.files) {
        $path = Join-Path $appDir $file.path
        if (-not (Test-Path -LiteralPath $path)) { $mismatch += "missing $($file.path)" }
        elseif ((Get-Sha256 $path) -ne $file.sha256) { $mismatch += "changed $($file.path)" }
    }
    Add-Check 'install.files' 'Every installed file is the package manifest''s (SHA-256)' ($mismatch.Count -eq 0) `
        ("{0} files{1}" -f @($manifest.files).Count, $(if ($mismatch) { '; ' + (($mismatch | Select-Object -First 5) -join '; ') } else { '' }))

    # --- 3. Lucent.exe: import, Auto, JPEG; then TIFF -------------------------------------------------
    $dataRoot = Join-Path $work 'data'
    $fixtureFolder = Join-Path $inputDir 'fixtures'
    function Invoke-Lucent([string]$Name, [string[]]$Arguments) {
        $stderr = Join-Path $results "$Name-stderr.log"
        $saved = @{}
        foreach ($variable in 'PATH', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_FORCE_STDERR_LOGGING', 'QT_QUICK_CONTROLS_STYLE') {
            $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process')
            [Environment]::SetEnvironmentVariable($variable, $null, 'Process')
        }
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
            $env:QT_FORCE_STDERR_LOGGING = '1'
            $quoted = @($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } })
            $process = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $work -PassThru -NoNewWindow `
                -RedirectStandardError $stderr -RedirectStandardOutput (Join-Path $results "$Name-stdout.log")
        }
        finally {
            foreach ($variable in $saved.Keys) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') }
        }
        $null = $process.Handle
        if (-not $process.WaitForExit($SettleTimeoutMs + 120000)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit() }
        # Lucent.exe writes UTF-8; plain strings, so the lines the result keeps serialise as text (SBX-1).
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1); Lines = (Read-TextLines $stderr) }
    }
    function Test-Run([string]$Prefix, [string]$Label, $Run, [string]$Screenshot) {
        Add-Check "$Prefix.exit" "$($Label): Lucent.exe exited with 0 after its screenshot" ($Run.ExitCode -eq 0) ("exit code {0}, {1} s" -f $Run.ExitCode, $Run.Seconds)
        $png = (Test-Path -LiteralPath $Screenshot) -and ([System.IO.File]::ReadAllBytes($Screenshot)[0..7] -join ',') -eq '137,80,78,71,13,10,26,10'
        Add-Check "$Prefix.screenshot" "$($Label): the window's screenshot was saved" $png $Screenshot
        $problems = @($Run.Lines | Where-Object { $_ -match 'module ".*" is not installed|Cannot load library|Could not (find|load) the Qt platform plugin|failed to load|qrc:.*(Error|error)|QQmlApplicationEngine failed|is not a type|ReferenceError|TypeError' })
        Add-Check "$Prefix.qt" "$($Label): no Qt or QML loading error" ($problems.Count -eq 0) (($problems | Select-Object -First 3) -join ' | ')
        $support = @($Run.Lines | Where-Object { $_ -match 'camera support database \S+ \(qualified' }) | Select-Object -First 1
        $lenses = @($Run.Lines | Where-Object { $_ -match '^lucent: lens database \S+ \(' }) | Select-Object -First 1
        $assets = Join-Path $appDir 'assets'
        $fromInstallation = [bool]$support -and [bool]$lenses -and $support.IndexOf($assets, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -and
            $lenses.IndexOf($assets, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        Add-Check "$Prefix.assets" "$($Label): the installed camera support and lens databases are active" $fromInstallation (@($support, $lenses) -join ' | ')
    }
    function Test-Outputs([string]$Prefix, [string]$Label, [string]$Folder, [string]$Kind, $Run) {
        $files = @(Get-ChildItem -LiteralPath $Folder -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '~lucent-*' } | Sort-Object Name)
        $invalid = New-Object System.Collections.Generic.List[string]
        foreach ($file in $files) {
            $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
            if ($Kind -eq 'jpeg') {
                if (-not ($bytes.Length -gt 1000 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xD8 -and $bytes[2] -eq 0xFF -and $bytes[-2] -eq 0xFF -and $bytes[-1] -eq 0xD9)) {
                    $invalid.Add("$($file.Name): not a JPEG (SOI and EOI markers)")
                }
                continue
            }
            # A 16-bit RGB TIFF, read from its first image file directory (W12 review CMG-04).
            $layout = Get-TiffLayout $bytes
            if ($layout.Problem) { $invalid.Add("$($file.Name): not a TIFF ($($layout.Problem))") }
            elseif ($bytes.Length -le 1000 -or $layout.SamplesPerPixel -ne 3 -or (@($layout.BitsPerSample) -join ',') -ne '16,16,16' -or $layout.PlanarConfiguration -ne 1) {
                $invalid.Add(("{0}: SamplesPerPixel {1}, BitsPerSample {2}, PlanarConfiguration {3}" -f $file.Name, $layout.SamplesPerPixel, (@($layout.BitsPerSample) -join ','), $layout.PlanarConfiguration))
            }
        }
        $completed = @($Run.Lines | Where-Object { $_ -match '^lucent: export completed: ' })
        $failed = @($Run.Lines | Where-Object { $_ -match '^lucent: export (failed|cancelled): ' })
        $valid = $files.Count - $invalid.Count
        Add-Check "$Prefix" ("{0}: every photo exported as {1}" -f $Label, $(if ($Kind -eq 'jpeg') { 'a valid JPEG' } else { 'a 16-bit RGB TIFF (BitsPerSample 16, 16, 16 in its first image file directory)' })) `
            ($files.Count -eq $fixtures.Count -and $valid -eq $fixtures.Count -and $completed.Count -eq $fixtures.Count -and $failed.Count -eq 0) `
            ("{0} photos, {1} files, {2} valid, {3} reported completed, {4} failed{5}{6}" -f $fixtures.Count, $files.Count, $valid, $completed.Count, $failed.Count,
                $(if ($failed) { ': ' + (($failed | Select-Object -First 2) -join ' | ') } else { '' }),
                $(if ($invalid.Count) { '; ' + (($invalid | Select-Object -First 2) -join ' | ') } else { '' }))
        return @($files | ForEach-Object { [ordered]@{ file = [string]$_.Name; bytes = $_.Length; sha256 = (Get-Sha256 $_.FullName) } })
    }

    Set-GateProgress 'run 1: import, Auto, JPEG export'
    $jpegFolder = Join-Path $work 'jpeg'
    New-Item -ItemType Directory -Path $jpegFolder | Out-Null   # --export-all needs an existing folder
    $shot1 = Join-Path $results 'lucent-auto-jpeg.png'
    $run1 = Invoke-Lucent 'run1' @('--data-root', $dataRoot, '--import', $fixtureFolder, '--include-subfolders', '--open-first', '--auto',
        '--export-all', $jpegFolder, '--export-format', 'jpeg', '--screenshot', $shot1, '--quit-after-screenshot', '--settle-timeout', "$SettleTimeoutMs")
    Test-Run 'run1' 'Run 1 (import, Auto, JPEG)' $run1 $shot1
    $autoStarted = @($run1.Lines | Where-Object { $_ -match '^lucent: auto: started on ' }) | Select-Object -First 1
    $autoFinished = @($run1.Lines | Where-Object { $_ -match '^lucent: auto: finished after \d+ ms: ' }) | Select-Object -First 1
    $autoSummary = @($run1.Lines | Where-Object { $_ -match '^lucent: auto: .*review recommended: (yes|no)$' }) | Select-Object -First 1
    Add-Check 'run1.auto' 'Run 1: Auto ran on the first photo and its automatic revision is current' ([bool]$autoStarted -and [bool]$autoFinished -and [bool]$autoSummary) `
        ((@($autoStarted, $autoFinished, $autoSummary) | Where-Object { $_ }) -join ' | ')
    # @(): a function's array output is unrolled (none: nothing; one: the record itself), and the result
    # must hold an array for every count (W12 review CMG-03).
    $jpegs = @(Test-Outputs 'run1.jpeg' 'Run 1' $jpegFolder 'jpeg' $run1)

    Set-GateProgress 'run 2: TIFF export of the same library'
    $tiffFolder = Join-Path $work 'tiff'
    New-Item -ItemType Directory -Path $tiffFolder | Out-Null
    $shot2 = Join-Path $results 'lucent-tiff.png'
    $run2 = Invoke-Lucent 'run2' @('--data-root', $dataRoot, '--export-all', $tiffFolder, '--export-format', 'tiff16', '--screenshot', $shot2,
        '--quit-after-screenshot', '--settle-timeout', "$SettleTimeoutMs")
    Test-Run 'run2' 'Run 2 (TIFF)' $run2 $shot2
    $tiffs = @(Test-Outputs 'run2.tiff' 'Run 2' $tiffFolder 'tiff' $run2)
    $changed = @($fixtures | Where-Object { (Get-Sha256 $_.FullName) -ne $fixtureHashes[$_.FullName] } | ForEach-Object Name)
    Add-Check 'fixtures.unchanged' 'The photos are unchanged (SHA-256)' ($changed.Count -eq 0) ($changed -join ', ')
    $outcome['runs'] = [ordered]@{
        run1 = [ordered]@{ exitCode = $run1.ExitCode; seconds = $run1.Seconds; auto = $(if ($autoSummary) { [string]$autoSummary } else { $null }); jpeg = $jpegs }
        run2 = [ordered]@{ exitCode = $run2.ExitCode; seconds = $run2.Seconds; tiff = $tiffs } }

    # --- 4. Uninstall ---------------------------------------------------------------------------------
    Set-GateProgress 'uninstall'
    Invoke-GateUninstall
    $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lucent.lnk'
    Add-Check 'uninstall.finished' 'The uninstaller finished' (-not (Test-Path -LiteralPath $uninstallKey)) ''
    $left = if (Test-Path -LiteralPath $appDir) { @(Get-ChildItem -LiteralPath $appDir -Recurse -Force | ForEach-Object FullName) } else { @() }
    Add-Check 'uninstall.removed' 'The program folder, the Start menu shortcut and the registration are gone' `
        (-not (Test-Path -LiteralPath $appDir) -and -not (Test-Path -LiteralPath $startMenu) -and -not (Test-Path -LiteralPath $uninstallKey)) (($left | Select-Object -First 5) -join ', ')
    Add-Check 'uninstall.data-kept' 'The library (data root) is kept' (Test-Path -LiteralPath (Join-Path $dataRoot 'data\lucent.catalog')) $dataRoot

    # --- 5. The installer test ------------------------------------------------------------------------
    if (-not $SkipInstallerTest) {
        Set-GateProgress 'installer test'
        $testWork = Join-Path $work 'installer-test'
        $test = Join-Path $inputDir 'scripts\Test-InstalledPackage.ps1'
        $testArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $test, '-Installer', $installer, '-Manifest', $manifestPath,
            '-Fixture', $fixtures[0].FullName, '-WorkDir', $testWork, '-RuntimeOutcomes')
        if ($rehearsalSetup.Count -gt 0) { $testArguments += @('-NewerInstaller', $rehearsalSetup[0].FullName) }
        $testOutput = Join-Path $results 'installer-test-output.txt'
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & powershell.exe @testArguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $testOutput | Out-Null
            $testExit = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $previous }
        $testResult = $null
        $copy = Join-Path $results 'installer-test'
        New-Item -ItemType Directory -Path $copy -Force | Out-Null
        if (Test-Path -LiteralPath $testWork -PathType Container) {
            foreach ($file in (Get-ChildItem -LiteralPath $testWork -File | Where-Object { $_.Extension -in '.json', '.log', '.txt', '.png' })) {
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $copy $file.Name) -Force
            }
            $resultPath = Join-Path $testWork 'result.json'
            if (Test-Path -LiteralPath $resultPath -PathType Leaf) { $testResult = [System.IO.File]::ReadAllText($resultPath) | ConvertFrom-Json }
        }
        $failed = @(if ($testResult) { $testResult.checks | Where-Object { -not $_.passed } | ForEach-Object { [string]$_.name } })
        Add-Check 'installer-test' ("Test-InstalledPackage.ps1 -RuntimeOutcomes{0} passed" -f $(if ($rehearsalSetup.Count -gt 0) { ' -NewerInstaller' } else { '' })) `
            ($testExit -eq 0 -and $testResult -and [bool]$testResult.passed) `
            ("exit code {0}; {1} checks, {2} failed{3}" -f $testExit, $(if ($testResult) { @($testResult.checks).Count } else { 0 }), $failed.Count,
                $(if ($failed) { ': ' + (($failed | Select-Object -First 3) -join '; ') } else { '' }))
    }
}
catch {
    Add-Check 'gate.error' 'The gate ran to its end' $false $_.Exception.Message
    # Never leave an installation behind (a rehearsal runs on a real user account).
    if ($script:appDir -and (Test-Path -LiteralPath $uninstallKey)) {
        try { Invoke-GateUninstall } catch { Write-Host "The uninstaller failed: $($_.Exception.Message)" -ForegroundColor Red }
    }
}
finally {
    $required = @($checks | Where-Object { -not ($Rehearsal -and $_.id -like 'environment.*') })
    $outcome['passed'] = ($checks.Count -gt 0 -and @($required | Where-Object { -not $_.passed }).Count -eq 0)
    $outcome['started'] = $started.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $outcome['finished'] = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $outcome['notes'] = $notes.ToArray()
    $outcome['checks'] = $checks.ToArray()
    try {
        Write-Json 'gate.json' $outcome
    }
    catch {
        # The host must always find a result: a failed one when the full record cannot be written.
        $outcome['passed'] = $false
        $failure = [ordered]@{
            gate = 'clean-machine'; format = 1; mode = $outcome['mode']; passed = $false
            checks = @([ordered]@{ id = 'gate.error'; name = 'The gate wrote its result'; passed = $false; detail = [string]$_.Exception.Message }) }
        $path = Join-Path $results 'gate.json'
        try {
            [System.IO.File]::WriteAllText("$path.partial", (ConvertTo-Json -InputObject $failure -Depth 4), $utf8)
            [System.IO.File]::Move("$path.partial", $path)
        }
        catch { [System.IO.File]::WriteAllText($path, (ConvertTo-Json -InputObject $failure -Depth 4), $utf8) }
    }
    Set-GateProgress 'done'
    Write-Host ''
    Write-Host ("Clean-machine gate ({0}): {1}. {2} of {3} checks passed. Results: {4}" -f $outcome.mode, $(if ($outcome.passed) { 'PASSED' } else { 'FAILED' }),
        @($checks | Where-Object { $_.passed }).Count, $checks.Count, $results) -ForegroundColor $(if ($outcome.passed) { 'Green' } else { 'Red' })
    try { Stop-Transcript | Out-Null } catch { }
    if ($Shutdown) {
        Write-Host 'Windows shuts down in 15 seconds, which closes the sandbox (its files are discarded; the results stay on the host).'
        Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\shutdown.exe') -ArgumentList @('/s', '/t', '15') -WindowStyle Hidden
    }
}
if ($outcome.passed) { exit 0 }
exit 1
