<#
.SYNOPSIS
    Installs Lucent Setup silently for the current user into a temporary folder, runs the installed
    Lucent.exe with a Windows-only PATH, upgrades it in place, uninstalls it and checks that nothing
    is left behind (docs/adr/0009-windows-packaging.md).

.DESCRIPTION
    1. Refuses to run when Lucent is already installed for this user, so a real installation is never
       touched. Records the state of every file-extension key under HKCU\Software\Classes and, for
       the RAW extensions of the package manifest, what Windows opens them with (AssocQueryString:
       command, program name, ProgID) and the per-user Explorer state (FileExts).
    2. Runs Setup with /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR=<work>\app and the
       tasks given (default: desktop shortcut and "Open with Lucent"), then checks the files (against
       the package manifest when given), shortcuts, uninstall registration and associations:
       Applications\Lucent.exe\SupportedTypes lists every RAW extension, OpenWithProgids names Lucent
       exactly for the extensions that already had a default program, and no extension's default
       value or effective program changed.
    3. Runs <work>\app\Lucent.exe with PATH=%SystemRoot%\System32;%SystemRoot%, without Qt variables,
       using its developer options: --data-root <work>\data --import <fixture> --open-first
       --screenshot <work>\lucent.png --quit-after-screenshot. Records every module the process loads.
       A module outside the installation folder and Windows fails the test if it has the name of a
       DLL Lucent ships or imports; other foreign modules (injected by software on the machine, for
       example a remote-desktop client) are reported.
    4. Upgrades in place while the installed Lucent.exe is running: plants files a "previous version"
       installed (listed in Lucent-files.txt, so the new Setup must delete them) and files of another
       program (not listed, so they must survive), then runs Setup again. Checks that Restart Manager
       closed Lucent before the cleanup, that exactly the listed stale files and their emptied
       folders are gone, the other program's files are intact, and the files match the manifest.
       The other program's files are then removed by the test.
    5. Runs the uninstaller silently and waits for it, then checks that the program folder, the
       shortcuts, the uninstall registration and every association value are gone, the extension
       keys, the effective associations and the Explorer state are as before, and the data root is
       still there.
    6. With -RuntimeOutcomes, also runs Setup with the test hook /LucentTestVCRedistExitCode=<code>
       (no runtime installer is started; Setup honours the hook only where the runtime is already
       installed): 1602, 1223, 1618 and start-failed:1223 (the elevation prompt declined) must stop
       Setup with exit code 7 and nothing installed; 0 and 1638 must install (after re-checking the
       runtime); 3010 must install, ask for a restart (exit code 3010 through /RESTARTEXITCODE) and
       offer no launch. Each installation is removed again.

    Writes result.json and logs into the work folder. Exit code 0 when every check passed.

.PARAMETER Installer
    Lucent-Setup-<version>-x64.exe.
.PARAMETER Fixture
    A RAW file to import.
.PARAMETER Manifest
    Optional package manifest (Lucent-Setup-<version>-x64.manifest.json): installed files are compared
    with it by SHA-256, and its RAW extension list drives the association checks (without it those
    checks use the extensions Setup registered).
.PARAMETER WorkDir
    Work folder (default %TEMP%\lucent-package-test-<time>). Must not exist yet.
.PARAMETER Tasks
    Setup tasks (default 'desktopicon,openwith').
.PARAMETER RuntimeOutcomes
    Also check Setup's handling of the runtime installer's outcomes (see DESCRIPTION).
.PARAMETER SettleTimeoutMs
    Passed to Lucent.exe --settle-timeout (default 120000).

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-InstalledPackage.ps1 `
        -Installer build\release\package\Lucent-Setup-0.1.0-x64.exe `
        -Manifest build\release\package\Lucent-Setup-0.1.0-x64.manifest.json `
        -Fixture fixtures\cache\canon-eos-r5-cr3-raw\Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3 -RuntimeOutcomes

.NOTES
    Needs no developer tools, so it can run on a clean Windows machine or in Windows Sandbox.
    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$Fixture,
    [string]$Manifest,
    [string]$WorkDir,
    [string]$Tasks = 'desktopicon,openwith',
    [switch]$RuntimeOutcomes,
    [int]$SettleTimeoutMs = 120000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$appGuid = '{ADBA4913-BBBD-42D8-90EF-0E9C80412232}'   # AppId in installer\lucent.iss
$progId = 'Lucent.RawPhoto'
$fileListName = 'Lucent-files.txt'                     # InstalledFileList in installer\lucent.iss
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($appGuid)_is1"
$classesKey = 'HKCU:\Software\Classes'
$fileExtsKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts'

$Installer = (Resolve-Path -LiteralPath $Installer).Path
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
if (-not $WorkDir) { $WorkDir = Join-Path $env:TEMP ('lucent-package-test-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $WorkDir) { throw "$WorkDir already exists." }
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
$appDir = Join-Path $WorkDir 'app'
$dataRoot = Join-Path $WorkDir 'data'
New-Item -ItemType Directory -Path $dataRoot | Out-Null
$manifestData = if ($Manifest) { Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json } else { $null }

$checks = New-Object System.Collections.Generic.List[object]
$notes = New-Object System.Collections.Generic.List[string]
function Add-Check([string]$Name, [bool]$Passed, [string]$Detail = '') {
    $checks.Add([pscustomobject]@{ Name = $Name; Passed = $Passed; Detail = $Detail })
    $mark = if ($Passed) { 'PASS' } else { 'FAIL' }
    $color = if ($Passed) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] {1}{2}" -f $mark, $Name, $(if ($Detail) { " - $Detail" } else { '' })) -ForegroundColor $color
}

$startMenuLink = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lucent.lnk'
$desktopLink = Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Lucent.lnk'
$userData = Join-Path $env:LOCALAPPDATA 'Lucent'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class LucentTestAssoc {
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
    static extern int AssocQueryStringW(int flags, int str, string assoc, string extra, StringBuilder outBuf, ref uint outLen);
    // What Windows opens a file type with: ASSOCSTR_COMMAND (1), ASSOCSTR_FRIENDLYAPPNAME (4) and
    // ASSOCSTR_PROGID (20), with ASSOCF_INIT_IGNOREUNKNOWN; an HRESULT when there is none.
    public static string Describe(string extension) {
        return Query(extension, 1) + " | " + Query(extension, 4) + " | " + Query(extension, 20);
    }
    static string Query(string extension, int what) {
        uint length = 4096; var text = new StringBuilder((int)length);
        int hr = AssocQueryStringW(0x400, what, extension, null, text, ref length);
        return hr == 0 ? text.ToString() : String.Format("0x{0:X8}", hr);
    }
}
'@

# --- Preconditions and snapshot ------------------------------------------------------------------

if (Test-Path -LiteralPath $uninstallKey) {
    throw "Lucent is already installed for this user ($((Get-ItemProperty -LiteralPath $uninstallKey).InstallLocation)). This test never touches a real installation; uninstall it first or use another account."
}
foreach ($link in $startMenuLink, $desktopLink) {
    if (Test-Path -LiteralPath $link) { throw "$link already exists; this test would remove it." }
}
foreach ($key in "$classesKey\$progId", "$classesKey\Applications\Lucent.exe") {
    if (Test-Path -LiteralPath $key) { throw "$key already exists; this test would remove it." }
}

# Every file-extension key of the user (HKCU\Software\Classes\.*), so the test needs no list of the
# extensions Setup registers and notices any key it creates, changes or leaves behind.
function Get-ExtensionState([string]$Extension) {
    $key = "$classesKey\.$Extension"
    if (-not (Test-Path -LiteralPath $key)) { return [pscustomobject]@{ Exists = $false; Default = $null; OpenWith = @() } }
    $default = (Get-Item -LiteralPath $key).GetValue('')
    $openWith = @()
    if (Test-Path -LiteralPath "$key\OpenWithProgids") { $openWith = @((Get-Item -LiteralPath "$key\OpenWithProgids").GetValueNames()) }
    return [pscustomobject]@{ Exists = $true; Default = $default; OpenWith = $openWith }
}
function Get-ExtensionNames {
    return @(Get-ChildItem -LiteralPath $classesKey -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName.StartsWith('.') } |
        ForEach-Object { $_.PSChildName.Substring(1).ToLowerInvariant() })
}
$before = @{}
foreach ($extension in (Get-ExtensionNames)) { $before[$extension] = Get-ExtensionState $extension }
function Get-BeforeState([string]$Extension) {
    if ($before.ContainsKey($Extension)) { return $before[$Extension] }
    return [pscustomobject]@{ Exists = $false; Default = $null; OpenWith = @() }
}

# The RAW extensions, what Windows opens each with, whether it has a default program (the rule of
# ExtensionHasDefaultProgram in lucent.iss) and the Explorer state of the type.
function Get-KeyText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '-' }
    $item = Get-Item -LiteralPath $Path
    $values = @($item.GetValueNames() | Sort-Object | ForEach-Object { "$_=$($item.GetValue($_))" }) -join ','
    $children = @(Get-ChildItem -LiteralPath $Path -ErrorAction SilentlyContinue | Sort-Object PSChildName | ForEach-Object { "$($_.PSChildName){$(Get-KeyText $_.PSPath)}" }) -join ';'
    return "[$values]$children"
}
function Test-HasDefaultProgram([string]$Extension) {
    foreach ($choice in "$fileExtsKey\$Extension\UserChoice", "$fileExtsKey\$Extension\UserChoiceLatest\ProgId") {
        if ((Test-Path -LiteralPath $choice) -and (Get-Item -LiteralPath $choice).GetValue('ProgId')) { return $true }
    }
    $root = [Microsoft.Win32.Registry]::ClassesRoot
    $key = $root.OpenSubKey($Extension)
    if (-not $key) { return $false }
    try { $default = $key.GetValue('') } finally { $key.Close() }
    if (-not $default -or $default -eq $progId) { return $false }
    $target = $root.OpenSubKey([string]$default)
    if ($target) { $target.Close(); return $true }
    return $false
}
$rawExtensions = @()
if ($manifestData -and ($manifestData.PSObject.Properties.Name -contains 'fileAssociations')) { $rawExtensions = @($manifestData.fileAssociations.extensions) }
$effectiveBefore = @{}; $explorerBefore = @{}; $hadDefault = @{}
foreach ($extension in $rawExtensions) {
    $effectiveBefore[$extension] = [LucentTestAssoc]::Describe($extension)
    $explorerBefore[$extension] = Get-KeyText "$fileExtsKey\$extension"
    $hadDefault[$extension] = Test-HasDefaultProgram $extension
}
if (-not $rawExtensions) { $notes.Add('No manifest with fileAssociations: the effective-association checks use the extensions Setup registered.') }
$fixtureExtension = [System.IO.Path]::GetExtension($Fixture).TrimStart('.').ToLowerInvariant()
$userDataBefore = if (Test-Path -LiteralPath $userData) { (Get-Item -LiteralPath $userData).LastWriteTimeUtc } else { $null }

function Invoke-Setup([string]$Directory, [string]$LogPath, [string[]]$Extra = @()) {
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=`"$Directory`"",
                   "/TASKS=`"$Tasks`"", "/LOG=`"$LogPath`"") + $Extra
    $process = Start-Process -FilePath $Installer -ArgumentList $arguments -PassThru -Wait
    return $process.ExitCode
}

function Get-UninstallerTempFolders {
    return @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'is-*-uninstall.tmp' -ErrorAction SilentlyContinue | ForEach-Object FullName)
}

function Invoke-Uninstall([string]$Directory, [string]$LogPath) {
    $uninstaller = Join-Path $Directory 'unins000.exe'
    if (-not (Test-Path -LiteralPath $uninstaller)) { return $false }
    $tempBefore = Get-UninstallerTempFolders
    Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$LogPath`"") -Wait | Out-Null
    # The uninstaller continues in a copy of itself in %TEMP%; wait until it has finished.
    $deadline = (Get-Date).AddSeconds(180)
    $finished = $false
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-Path -LiteralPath $uninstallKey) -and -not (Test-Path -LiteralPath $uninstaller)) { Start-Sleep -Seconds 2; $finished = $true; break }
        Start-Sleep -Milliseconds 500
    }
    # Inno Setup leaves that copy's folder (marked _unins-done.tmp when finished) for its next
    # uninstaller run to delete; remove the ones this uninstallation left.
    foreach ($folder in (Get-UninstallerTempFolders | Where-Object { $tempBefore -notcontains $_ })) {
        for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $folder); $attempt++) {
            if (Test-Path -LiteralPath (Join-Path $folder '_unins-done.tmp')) { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
            if (Test-Path -LiteralPath $folder) { Start-Sleep -Milliseconds 500 }
        }
        if (Test-Path -LiteralPath $folder) { $notes.Add("Inno Setup's uninstaller left $folder") }
    }
    return $finished
}

# --- 1. Install ----------------------------------------------------------------------------------

Write-Host "Work folder: $WorkDir"
$installLog = Join-Path $WorkDir 'install.log'
$installTimer = [System.Diagnostics.Stopwatch]::StartNew()
$exitCode = Invoke-Setup -Directory $appDir -LogPath $installLog
Add-Check 'Setup exit code 0' ($exitCode -eq 0) "exit code $exitCode, $([int]$installTimer.Elapsed.TotalSeconds) s"
$exe = Join-Path $appDir 'Lucent.exe'
Add-Check 'Lucent.exe installed into the chosen folder' (Test-Path -LiteralPath $exe) $exe
# W7 (ADR 0012): exports and batches run in lucent-worker.exe, which Lucent.exe starts from its own folder.
$workerExe = Join-Path $appDir 'lucent-worker.exe'
Add-Check 'lucent-worker.exe installed next to Lucent.exe' (Test-Path -LiteralPath $workerExe) $workerExe
$installText = if (Test-Path -LiteralPath $installLog) { Get-Content -LiteralPath $installLog -Raw } else { '' }
$runtimeLine = ($installText -split "`r?`n" | Where-Object { $_ -match 'VC\+\+ runtime:' }) -join ' | '
Add-Check 'Runtime check logged' ($runtimeLine -ne '') $runtimeLine
Add-Check 'Setup ran without elevation (non administrative install mode)' ($installText -match 'Administrative install mode: No|Non administrative install mode') ''
Add-Check 'A fresh installation removes nothing (no previous installed-file list)' ($installText -notmatch 'Upgrade: removed') ''

function Test-InstalledFiles([string]$Name) {
    if (-not $manifestData) { return }
    $expected = $manifestData.files
    $mismatch = @()
    foreach ($file in $expected) {
        $path = Join-Path $appDir $file.path
        if (-not (Test-Path -LiteralPath $path)) { $mismatch += "missing $($file.path)"; continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { $mismatch += "changed $($file.path)" }
    }
    $installed = @(Get-ChildItem -LiteralPath $appDir -Recurse -File | Where-Object { $_.Name -notmatch '^unins\d{3}\.(exe|dat)$' })
    Add-Check $Name ($mismatch.Count -eq 0 -and $installed.Count -eq @($expected).Count) `
        ("{0} files expected, {1} installed{2}" -f @($expected).Count, $installed.Count, $(if ($mismatch) { '; ' + (($mismatch | Select-Object -First 5) -join '; ') } else { '' }))
}
Test-InstalledFiles 'Installed files match the package manifest (SHA-256)'
$listPath = Join-Path $appDir $fileListName
$listed = if (Test-Path -LiteralPath $listPath) { @(Get-Content -LiteralPath $listPath | Where-Object { $_ -and -not $_.StartsWith(';') }) } else { @() }
$present = @(Get-ChildItem -LiteralPath $appDir -Recurse -File | Where-Object { $_.Name -notmatch '^unins\d{3}\.(exe|dat)$' } |
    ForEach-Object { $_.FullName.Substring($appDir.Length + 1) })
Add-Check "$fileListName lists exactly the installed files" ($listed.Count -gt 0 -and @(Compare-Object $listed $present).Count -eq 0) "$($listed.Count) listed, $($present.Count) installed"

$registration = if (Test-Path -LiteralPath $uninstallKey) { Get-ItemProperty -LiteralPath $uninstallKey } else { $null }
Add-Check 'Per-user uninstall registration (HKCU)' ([bool]$registration) $(if ($registration) { "$($registration.DisplayName) $($registration.DisplayVersion)" } else { '' })

$shell = New-Object -ComObject WScript.Shell
foreach ($link in @(@{ Path = $startMenuLink; Name = 'Start Menu shortcut' }, @{ Path = $desktopLink; Name = 'Desktop shortcut' })) {
    if ($link.Name -eq 'Desktop shortcut' -and $Tasks -notmatch 'desktopicon') { continue }
    $ok = Test-Path -LiteralPath $link.Path
    $target = if ($ok) { $shell.CreateShortcut($link.Path).TargetPath } else { '' }
    Add-Check "$($link.Name) points to the installed Lucent.exe" ($ok -and $target -ieq $exe) $target
}

$associated = @()
$supported = @()
if ($Tasks -match 'openwith') {
    $command = (Get-Item -LiteralPath "$classesKey\$progId\shell\open\command" -ErrorAction SilentlyContinue)
    $commandText = if ($command) { $command.GetValue('') } else { '' }
    Add-Check 'ProgID Lucent.RawPhoto opens the installed Lucent.exe' ($commandText -eq "`"$exe`" `"%1`"") $commandText
    $supportedKey = "$classesKey\Applications\Lucent.exe\SupportedTypes"
    $supported = if (Test-Path -LiteralPath $supportedKey) { @((Get-Item -LiteralPath $supportedKey).GetValueNames() | Sort-Object) } else { @() }
    if (-not $rawExtensions) {
        $rawExtensions = $supported
        foreach ($extension in $rawExtensions) { $hadDefault[$extension] = $null }
    }
    $missing = @($rawExtensions | Where-Object { $supported -notcontains $_ })
    Add-Check '"Open with Lucent": every RAW extension listed in Applications\Lucent.exe\SupportedTypes' `
        ($supported.Count -ge 10 -and $missing.Count -eq 0 -and $supported -contains ".$fixtureExtension") ("{0} listed{1}" -f $supported.Count, $(if ($missing) { '; missing ' + ($missing -join ' ') } else { '' }))
    $unchanged = $true
    $details = @()
    foreach ($extension in (Get-ExtensionNames)) {
        $state = Get-ExtensionState $extension
        if ($state.OpenWith -contains $progId) { $associated += ".$extension" }
        if ($state.Default -ne (Get-BeforeState $extension).Default) { $unchanged = $false; $details += ".$extension default changed" }
    }
    $wrong = @($rawExtensions | Where-Object { $null -ne $hadDefault[$_] -and (($associated -contains $_) -ne [bool]$hadDefault[$_]) })
    $withDefault = @($rawExtensions | Where-Object { $hadDefault[$_] }).Count
    Add-Check 'OpenWithProgids names Lucent exactly for the RAW extensions that had a default program' ($wrong.Count -eq 0) `
        ("{0} of {1} had one, {2} got the value{3}" -f $withDefault, $rawExtensions.Count, $associated.Count, $(if ($wrong) { '; wrong: ' + ($wrong -join ' ') } else { '' }))
    Add-Check 'No extension default value changed' $unchanged ($details -join '; ')
}
$changedEffective = @($rawExtensions | Where-Object { $effectiveBefore.ContainsKey($_) -and [LucentTestAssoc]::Describe($_) -ne $effectiveBefore[$_] } |
    ForEach-Object { "$_ : $($effectiveBefore[$_]) -> $([LucentTestAssoc]::Describe($_))" })
Add-Check 'What Windows opens each RAW type with is unchanged (AssocQueryString)' ($changedEffective.Count -eq 0) (($changedEffective | Select-Object -First 3) -join ' / ')

# --- 2. Run the installed Lucent.exe --------------------------------------------------------------

$screenshot = Join-Path $WorkDir 'lucent.png'
$stderrPath = Join-Path $WorkDir 'lucent-stderr.log'
$savedEnvironment = @{}
foreach ($name in 'PATH', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_FORCE_STDERR_LOGGING', 'QT_QUICK_CONTROLS_STYLE') {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
$modules = @{}
$runTimer = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $arguments = @('--data-root', "`"$dataRoot`"", '--import', "`"$Fixture`"", '--open-first',
                   '--screenshot', "`"$screenshot`"", '--quit-after-screenshot', '--settle-timeout', "$SettleTimeoutMs")
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $WorkDir -PassThru -NoNewWindow `
        -RedirectStandardError $stderrPath -RedirectStandardOutput (Join-Path $WorkDir 'lucent-stdout.log')
}
finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
$null = $process.Handle   # keeps the exit code available after the process ends
while (-not $process.HasExited) {
    try {
        foreach ($module in (Get-Process -Id $process.Id -ErrorAction Stop).Modules) { $modules[$module.FileName.ToLowerInvariant()] = $module.FileName }
    }
    catch { }
    Start-Sleep -Milliseconds 100
    if ($runTimer.Elapsed.TotalSeconds -gt ($SettleTimeoutMs / 1000 + 60)) { Stop-Process -Id $process.Id -Force; break }
}
$process.WaitForExit()
Add-Check 'Installed Lucent.exe ran and exited with 0 after the screenshot' ($process.ExitCode -eq 0) ("exit code {0}, {1:N1} s" -f $process.ExitCode, $runTimer.Elapsed.TotalSeconds)
Add-Check 'Screenshot written' (Test-Path -LiteralPath $screenshot) $screenshot
$stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath } else { @() }
$qtProblems = @($stderr | Where-Object { $_ -match 'module ".*" is not installed|Cannot load library|Could not (find|load) the Qt platform plugin|failed to load|qrc:.*(Error|error)|QQmlApplicationEngine failed' })
Add-Check 'No Qt/QML loading errors on stderr' ($qtProblems.Count -eq 0) (($qtProblems | Select-Object -First 3) -join ' | ')

$appPrefix = $appDir.ToLowerInvariant().TrimEnd('\') + '\'
$windowsPrefix = $env:SystemRoot.ToLowerInvariant().TrimEnd('\') + '\'
$shipped = @{}
foreach ($file in Get-ChildItem -LiteralPath $appDir -Recurse -File -Filter '*.dll') { $shipped[$file.Name.ToLowerInvariant()] = $true }
$fromApp = @(); $fromWindows = @(); $foreign = @(); $wrongCopies = @()
foreach ($key in ($modules.Keys | Sort-Object)) {
    if ($key.StartsWith($appPrefix)) { $fromApp += $modules[$key] }
    elseif ($key.StartsWith($windowsPrefix)) { $fromWindows += $modules[$key] }
    else {
        $foreign += $modules[$key]
        if ($shipped.ContainsKey([System.IO.Path]::GetFileName($key))) { $wrongCopies += $modules[$key] }
    }
}
Add-Check 'Every Lucent and Qt module came from the installation folder' ($fromApp.Count -gt 10 -and $wrongCopies.Count -eq 0) `
    ("{0} from the installation folder, {1} from Windows, {2} other{3}" -f $fromApp.Count, $fromWindows.Count, $foreign.Count,
        $(if ($wrongCopies) { '; WRONG COPIES: ' + ($wrongCopies -join ', ') } else { '' }))
foreach ($module in $foreign) { $notes.Add("Module from outside the installation folder and Windows (not a Lucent dependency): $module") }
$vcModules = @($fromWindows | Where-Object { [System.IO.Path]::GetFileName($_) -match '^(msvcp140|vcruntime140|concrt140)' })
$notes.Add('Visual C++ runtime loaded from: ' + (($vcModules | ForEach-Object { $_ }) -join ', '))

# --- Upgrade in place, with Lucent running ------------------------------------------------------

# Files a previous version installed but this one does not ship: listed in the installed-file list,
# so the upgrade must delete them (and the folders they leave empty).
$stale = @('obsolete-test.dll', 'plugins\obsoletetest\qobsolete.dll', 'qml\ObsoleteTest\Inner\qmldir', 'licenses\obsolete-test\copyright.txt')
# Files of another program sharing the folder: not listed, so they must survive.
$foreignFiles = @('otherapp.dll', 'plugins\otherapp\codec.dll', 'qml\OtherApp\qmldir', 'licenses\otherapp-LICENSE.txt', 'qml\ObsoleteTest\otherapp-note.txt')
foreach ($relative in @($stale) + @($foreignFiles)) {
    $path = Join-Path $appDir $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [System.IO.File]::WriteAllText($path, "test file $relative")
}
[System.IO.File]::AppendAllText($listPath, (($stale -join "`r`n") + "`r`n"), (New-Object System.Text.ASCIIEncoding))

# Lucent running from the installation folder: Restart Manager must close it before the upgrade
# removes anything.
$running = Start-Process -FilePath $exe -ArgumentList @('--data-root', "`"$(Join-Path $WorkDir 'data-upgrade')`"") -WorkingDirectory $WorkDir -PassThru `
    -RedirectStandardError (Join-Path $WorkDir 'running-stderr.log') -RedirectStandardOutput (Join-Path $WorkDir 'running-stdout.log')
$deadline = (Get-Date).AddSeconds(60)
while (-not $running.HasExited -and (Get-Date) -lt $deadline) {
    $running.Refresh()
    if ($running.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
}
$wasRunning = -not $running.HasExited
Start-Sleep -Seconds 2
$reinstallLog = Join-Path $WorkDir 'reinstall.log'
$exitCode = Invoke-Setup -Directory $appDir -LogPath $reinstallLog
$closed = $running.WaitForExit(30000)
if (-not $closed) { Stop-Process -Id $running.Id -Force -ErrorAction SilentlyContinue }
$reinstallLines = if (Test-Path -LiteralPath $reinstallLog) { @(Get-Content -LiteralPath $reinstallLog) } else { @() }
function Get-LineIndex([string]$Pattern) {
    for ($i = 0; $i -lt $reinstallLines.Count; $i++) { if ($reinstallLines[$i] -match $Pattern) { return $i } }
    return -1
}
$shutdownAt = Get-LineIndex 'Shutting down applications using our files'
$cleanupAt = Get-LineIndex 'Upgrade: removed \d+ files'
Add-Check 'Upgrade over the running installation: Setup exit code 0' ($exitCode -eq 0) "exit code $exitCode"
Add-Check 'Restart Manager closed the running Lucent before the upgrade cleanup' ($wasRunning -and $closed -and $shutdownAt -ge 0 -and $cleanupAt -gt $shutdownAt) `
    ("running before: {0}; closed: {1}; log lines: shutdown {2}, cleanup {3}" -f $wasRunning, $closed, $shutdownAt, $cleanupAt)
$left = @($stale | Where-Object { Test-Path -LiteralPath (Join-Path $appDir $_) })
$emptyLeft = @('plugins\obsoletetest', 'qml\ObsoleteTest\Inner', 'licenses\obsolete-test') | Where-Object { Test-Path -LiteralPath (Join-Path $appDir $_) }
Add-Check 'Upgrade removed the files the previous version listed and this one does not ship, and their empty folders' ($left.Count -eq 0 -and @($emptyLeft).Count -eq 0) `
    (@($left) + @($emptyLeft) -join ', ')
$lost = @($foreignFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $appDir $_)) })
Add-Check "Upgrade kept another program's files in the same folders (root, plugins, qml, licenses)" ($lost.Count -eq 0) ($lost -join ', ')
foreach ($relative in $foreignFiles) { Remove-Item -LiteralPath (Join-Path $appDir $relative) -Force -ErrorAction SilentlyContinue }
foreach ($folder in 'qml\ObsoleteTest', 'qml\OtherApp', 'plugins\otherapp') {
    $path = Join-Path $appDir $folder
    if ((Test-Path -LiteralPath $path) -and -not (Get-ChildItem -LiteralPath $path -Recurse -File)) { Remove-Item -LiteralPath $path -Recurse -Force }
}
Test-InstalledFiles 'Files after the upgrade match the package manifest (SHA-256)'

# --- 3. Uninstall --------------------------------------------------------------------------------

$uninstalled = Invoke-Uninstall -Directory $appDir -LogPath (Join-Path $WorkDir 'uninstall.log')
Add-Check 'Uninstaller finished' $uninstalled ''
$leftovers = if (Test-Path -LiteralPath $appDir) { @(Get-ChildItem -LiteralPath $appDir -Recurse -Force | ForEach-Object FullName) } else { @() }
Add-Check 'Program folder removed' (-not (Test-Path -LiteralPath $appDir)) (($leftovers | Select-Object -First 5) -join ', ')
Add-Check 'Start Menu shortcut removed' (-not (Test-Path -LiteralPath $startMenuLink)) ''
Add-Check 'Desktop shortcut removed' (-not (Test-Path -LiteralPath $desktopLink)) ''
Add-Check 'Uninstall registration removed' (-not (Test-Path -LiteralPath $uninstallKey)) ''
Add-Check 'ProgID and Applications\Lucent.exe keys removed' (-not (Test-Path -LiteralPath "$classesKey\$progId") -and -not (Test-Path -LiteralPath "$classesKey\Applications\Lucent.exe")) ''
$restored = $true
$details = @()
foreach ($extension in (@(Get-ExtensionNames) + @($before.Keys) | Sort-Object -Unique)) {
    $state = Get-ExtensionState $extension
    $was = Get-BeforeState $extension
    if ($state.Exists -ne $was.Exists -or $state.Default -ne $was.Default -or ($state.OpenWith -contains $progId) -or
        (@($state.OpenWith | Sort-Object) -join ',') -ne (@($was.OpenWith | Sort-Object) -join ',')) {
        $restored = $false; $details += ".$extension"
    }
}
Add-Check 'Extension keys back to their state before the installation' $restored ($details -join ', ')
$changedEffective = @($rawExtensions | Where-Object { $effectiveBefore.ContainsKey($_) -and [LucentTestAssoc]::Describe($_) -ne $effectiveBefore[$_] })
$changedExplorer = @($rawExtensions | Where-Object { $explorerBefore.ContainsKey($_) -and (Get-KeyText "$fileExtsKey\$_") -ne $explorerBefore[$_] })
Add-Check 'Effective associations and Explorer state (FileExts) of the RAW types as before' ($changedEffective.Count -eq 0 -and $changedExplorer.Count -eq 0) `
    ((@($changedEffective | ForEach-Object { "$_ (association)" }) + @($changedExplorer | ForEach-Object { "$_ (FileExts)" })) -join ', ')
Add-Check 'Data root kept (catalog, settings, caches)' (Test-Path -LiteralPath (Join-Path $dataRoot 'data\lucent.catalog')) $dataRoot
$userDataAfter = if (Test-Path -LiteralPath $userData) { (Get-Item -LiteralPath $userData).LastWriteTimeUtc } else { $null }
Add-Check '%LOCALAPPDATA%\Lucent untouched by the test run' ($userDataAfter -eq $userDataBefore) ''

# --- 4. Runtime installer outcomes (test hook) ---------------------------------------------------

if ($RuntimeOutcomes) {
    foreach ($code in '1602', '1223', '1618', 'start-failed:1223') {
        $label = $code.Replace(':', '-')
        $directory = Join-Path $WorkDir "app-$label"
        $log = Join-Path $WorkDir "install-$label.log"
        $exitCode = Invoke-Setup -Directory $directory -LogPath $log -Extra @("/LucentTestVCRedistExitCode=$code")
        $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
        $stopLine = ($text -split "`r?`n" | Where-Object { $_ -match 'Setup stops' } | Select-Object -First 1)
        $nothing = -not (Test-Path -LiteralPath (Join-Path $directory 'Lucent.exe')) -and -not (Test-Path -LiteralPath $uninstallKey)
        Add-Check "Runtime installer outcome $($code): Setup stops (exit 7), nothing installed" ($exitCode -eq 7 -and $nothing -and [bool]$stopLine -and $text -match 'test hook, simulated') "exit code $exitCode; $stopLine"
        if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    }
    foreach ($code in '0', '1638') {
        $directory = Join-Path $WorkDir "app-$code"
        $log = Join-Path $WorkDir "install-$code.log"
        $exitCode = Invoke-Setup -Directory $directory -LogPath $log -Extra @("/LucentTestVCRedistExitCode=$code")
        $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
        $rechecked = ([regex]::Matches($text, 'VC\+\+ runtime: (64|32)-bit registration')).Count -ge 2
        Add-Check "Runtime installer outcome $($code): installs after re-checking the runtime" ($exitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $directory 'Lucent.exe')) -and $rechecked -and $text -match 'test hook, simulated') "exit code $exitCode"
        $removed = Invoke-Uninstall -Directory $directory -LogPath (Join-Path $WorkDir "uninstall-$code.log")
        Add-Check "Runtime installer outcome $($code): installation removed again" ($removed -and -not (Test-Path -LiteralPath (Join-Path $directory 'Lucent.exe')) -and -not (Test-Path -LiteralPath $startMenuLink)) ''
    }
    $directory = Join-Path $WorkDir 'app-3010'
    $log = Join-Path $WorkDir 'install-3010.log'
    $exitCode = Invoke-Setup -Directory $directory -LogPath $log -Extra @('/LucentTestVCRedistExitCode=3010', '/RESTARTEXITCODE=3010')
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Add-Check 'Runtime installer outcome 3010: installs and requests a restart' ($exitCode -eq 3010 -and (Test-Path -LiteralPath (Join-Path $directory 'Lucent.exe')) -and $text -match 'Windows must restart') "exit code $exitCode"
    Add-Check 'Runtime installer outcome 3010: no restart performed (/NORESTART)' ($text -match 'Need to restart Windows\? Yes' -and $text -match 'Will not restart Windows automatically') ''
    $removed = Invoke-Uninstall -Directory $directory -LogPath (Join-Path $WorkDir 'uninstall-3010.log')
    Add-Check 'Runtime installer outcome 3010: installation removed again' ($removed -and -not (Test-Path -LiteralPath (Join-Path $directory 'Lucent.exe')) -and -not (Test-Path -LiteralPath $startMenuLink)) ''
    $changedEffective = @($rawExtensions | Where-Object { $effectiveBefore.ContainsKey($_) -and [LucentTestAssoc]::Describe($_) -ne $effectiveBefore[$_] })
    $restoredAll = -not (Test-Path -LiteralPath "$classesKey\$progId") -and -not (Test-Path -LiteralPath "$classesKey\Applications\Lucent.exe") -and $changedEffective.Count -eq 0
    Add-Check 'After the runtime-outcome installations: no ProgID, Applications key or association change left' $restoredAll ($changedEffective -join ', ')
}

# --- Result --------------------------------------------------------------------------------------

foreach ($note in $notes) { Write-Host "NOTE: $note" }
$failed = @($checks | Where-Object { -not $_.Passed })
$result = [ordered]@{
    installer = $Installer; fixture = $Fixture; workDir = $WorkDir; tasks = $Tasks
    passed = ($failed.Count -eq 0); notes = @($notes)
    checks = @($checks | ForEach-Object { [ordered]@{ name = $_.Name; passed = $_.Passed; detail = $_.Detail } })
    modulesFromInstallation = @($fromApp); modulesFromWindows = @($fromWindows); modulesOther = @($foreign)
    openWithProgidsExtensions = @($associated); supportedTypes = @($supported)
}
$json = ConvertTo-Json -InputObject $result -Depth 6
[System.IO.File]::WriteAllText((Join-Path $WorkDir 'result.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("{0} of {1} checks passed. Details: {2}" -f ($checks.Count - $failed.Count), $checks.Count, (Join-Path $WorkDir 'result.json'))
if ($failed.Count -gt 0) { exit 1 }
exit 0
