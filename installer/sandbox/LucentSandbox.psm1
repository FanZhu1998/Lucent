#Requires -Version 5.1
<#
.SYNOPSIS
    The clean-machine gate's host side (tools\Test-CleanMachine.ps1, docs\release\clean-machine.md;
    blueprint section 10, "Offline installation").

.DESCRIPTION
    - New-LucentSandboxConfiguration   the Windows Sandbox configuration (.wsb) of a prepared gate
                                       folder: networking, the virtual GPU, the clipboard, printers,
                                       audio and video input off; the input folder read-only at
                                       C:\LucentGate\input, the results folder writable at
                                       C:\LucentGate\results; at logon the gate
                                       (installer\sandbox\Invoke-CleanMachineGate.ps1).
    - Test-LucentSandboxConfiguration  checks a .wsb: well-formed, valid against wsb-configuration.xsd
                                       (Microsoft's documented elements and values), and the gate's
                                       rules (networking disabled, exactly the read-only input and the
                                       writable results folder, the logon command runs the gate).
    - Test-CleanMachineResult          checks the gate.json a gate wrote against what the host
                                       prepared: every check the gate makes present and passed (the
                                       environment's only on a clean machine), one recorded export (a
                                       file with its SHA-256) per photo in each format, the photos the
                                       host gave, the package's installer, and the version rehearsal's
                                       upgrade and refused downgrade when the host gave its Setup.
    - Read-CleanMachineJson            reads a JSON file the sandbox writes, retrying while it is in
                                       use or incomplete (W12 sandbox run SBX-2).
    - Get-LucentSandboxInstances       the Windows Sandbox environments running on this computer
                                       (Windows Sandbox runs one at a time; W12 review CMG-02).
    - Get-LucentSandboxHostFacts       the host's Windows (build and update revision) and the Windows
                                       Sandbox in use (the app's version, or the Windows feature's).
    - $CleanMachineGateChecks          the ids of the gate's checks.

    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Where the gate's folders are inside the sandbox.
$script:SandboxRoot = 'C:\LucentGate'
# The ids of the checks Invoke-CleanMachineGate.ps1 makes; "installer-test" unless it ran with
# -SkipInstallerTest. The environment's two are required in Windows Sandbox only.
$script:GateCheckIds = @('environment.clean', 'environment.offline', 'install.exit', 'install.registration', 'install.files',
    'run1.exit', 'run1.screenshot', 'run1.qt', 'run1.assets', 'run1.auto', 'run1.jpeg',
    'run2.exit', 'run2.screenshot', 'run2.qt', 'run2.assets', 'run2.tiff', 'fixtures.unchanged',
    'uninstall.finished', 'uninstall.removed', 'uninstall.data-kept', 'installer-test')

function ConvertTo-XmlText([string]$Text) { return [System.Security.SecurityElement]::Escape($Text) }

function New-LucentSandboxConfiguration {
    <#
    .SYNOPSIS
        The .wsb text for the gate folders -InputDir (mapped read-only) and -ResultsDir (writable).
        -GateArguments are added to the logon command; -Manual opens Explorer at the input instead.
    #>
    param([Parameter(Mandatory)][string]$InputDir, [Parameter(Mandatory)][string]$ResultsDir, [ValidateRange(2048, 65536)][int]$MemoryMB = 4096,
          [string[]]$GateArguments = @(), [switch]$Manual)
    foreach ($folder in $InputDir, $ResultsDir) {
        if (-not [System.IO.Path]::IsPathRooted($folder) -or $folder -notmatch '^[A-Za-z]:\\') { throw "$folder is not an absolute folder on a drive." }
        if ($folder -match '[<>"|?*]') { throw "$folder has a character a mapped folder cannot have." }
    }
    $gate = "$script:SandboxRoot\input\scripts\Invoke-CleanMachineGate.ps1"
    foreach ($argument in $GateArguments) { if ($argument -match '[<>"&|]') { throw "The gate argument '$argument' has a character the logon command cannot carry." } }
    $command = if ($Manual) { "explorer.exe $script:SandboxRoot\input" } else {
        ((@('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $gate, '-Root', $script:SandboxRoot) + $GateArguments) -join ' ') }
    $lines = @(
        '<!-- Lucent clean-machine gate (tools\Test-CleanMachine.ps1, docs\release\clean-machine.md): offline, the package read-only, the results writable. -->',
        '<Configuration>',
        '  <Networking>Disable</Networking>',
        '  <vGPU>Disable</vGPU>',
        '  <ClipboardRedirection>Disable</ClipboardRedirection>',
        '  <PrinterRedirection>Disable</PrinterRedirection>',
        '  <AudioInput>Disable</AudioInput>',
        '  <VideoInput>Disable</VideoInput>',
        "  <MemoryInMB>$MemoryMB</MemoryInMB>",
        '  <MappedFolders>',
        '    <MappedFolder>',
        "      <HostFolder>$(ConvertTo-XmlText $InputDir)</HostFolder>",
        "      <SandboxFolder>$script:SandboxRoot\input</SandboxFolder>",
        '      <ReadOnly>true</ReadOnly>',
        '    </MappedFolder>',
        '    <MappedFolder>',
        "      <HostFolder>$(ConvertTo-XmlText $ResultsDir)</HostFolder>",
        "      <SandboxFolder>$script:SandboxRoot\results</SandboxFolder>",
        '      <ReadOnly>false</ReadOnly>',
        '    </MappedFolder>',
        '  </MappedFolders>',
        '  <LogonCommand>',
        "    <Command>$(ConvertTo-XmlText $command)</Command>",
        '  </LogonCommand>',
        '</Configuration>')
    return (($lines -join "`r`n") + "`r`n")
}

function Test-LucentSandboxConfiguration {
    <#
    .SYNOPSIS
        Checks the .wsb at -Path (see the module description). -InputDir and -ResultsDir, when given,
        must be the mapped host folders. -Manual: the logon command opens Explorer instead of the gate.
        Returns Valid and Problems.
    #>
    param([Parameter(Mandatory)][string]$Path, [string]$InputDir, [string]$ResultsDir, [switch]$Manual)
    $problems = New-Object System.Collections.Generic.List[string]
    $schemaPath = Join-Path $PSScriptRoot 'wsb-configuration.xsd'
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.ValidationType = [System.Xml.ValidationType]::Schema
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $schemaReader = [System.Xml.XmlReader]::Create($schemaPath)
    try { [void]$settings.Schemas.Add($null, $schemaReader) } finally { $schemaReader.Close() }
    $settings.add_ValidationEventHandler({ param($sender, $e) $problems.Add(("schema, line {0}: {1}" -f $e.Exception.LineNumber, $e.Message)) })
    $reader = [System.Xml.XmlReader]::Create($Path, $settings)
    try {
        while ($reader.Read()) { }
    }
    catch [System.Xml.XmlException] {
        $problems.Add("not well-formed XML: $($_.Exception.Message)")
    }
    finally {
        $reader.Close()
    }
    if ($problems.Count -eq 0) {
        [xml]$document = [System.IO.File]::ReadAllText($Path)
        $configuration = $document.Configuration
        $networking = $configuration.SelectSingleNode('Networking')
        if (-not $networking -or $networking.InnerText -ne 'Disable') { $problems.Add('networking is not disabled: the gate runs offline') }
        $mapped = @($configuration.SelectNodes('MappedFolders/MappedFolder'))
        $inputs = @($mapped | Where-Object { $_.SelectSingleNode('SandboxFolder') -and $_.SandboxFolder -eq "$script:SandboxRoot\input" })
        $outputs = @($mapped | Where-Object { $_.SelectSingleNode('SandboxFolder') -and $_.SandboxFolder -eq "$script:SandboxRoot\results" })
        if ($mapped.Count -ne 2 -or $inputs.Count -ne 1 -or $outputs.Count -ne 1) {
            $problems.Add("the gate maps exactly two folders, $script:SandboxRoot\input and $script:SandboxRoot\results (found $($mapped.Count))")
        }
        if ($inputs.Count -eq 1 -and (-not $inputs[0].SelectSingleNode('ReadOnly') -or $inputs[0].ReadOnly -ne 'true')) { $problems.Add('the input folder is not read-only') }
        if ($outputs.Count -eq 1 -and $outputs[0].SelectSingleNode('ReadOnly') -and $outputs[0].ReadOnly -ne 'false') { $problems.Add('the results folder is not writable') }
        if ($InputDir -and $inputs.Count -eq 1 -and $inputs[0].HostFolder -ne $InputDir) { $problems.Add("the input folder maps $($inputs[0].HostFolder), not $InputDir") }
        if ($ResultsDir -and $outputs.Count -eq 1 -and $outputs[0].HostFolder -ne $ResultsDir) { $problems.Add("the results folder maps $($outputs[0].HostFolder), not $ResultsDir") }
        foreach ($folder in $mapped) {
            if (-not (Test-Path -LiteralPath $folder.HostFolder -PathType Container)) { $problems.Add("the host folder $($folder.HostFolder) does not exist") }
        }
        $command = $configuration.SelectSingleNode('LogonCommand/Command')
        if (-not $command) { $problems.Add('there is no logon command') }
        elseif ($Manual) { if ($command.InnerText -notmatch '^explorer\.exe ') { $problems.Add('the manual configuration''s logon command does not open Explorer') } }
        elseif ($command.InnerText -notmatch '^powershell\.exe .*-File C:\\LucentGate\\input\\scripts\\Invoke-CleanMachineGate\.ps1 -Root C:\\LucentGate(\s|$)') {
            $problems.Add('the logon command does not run the gate (installer\sandbox\Invoke-CleanMachineGate.ps1) from the input folder')
        }
    }
    return [pscustomobject]@{ Valid = ($problems.Count -eq 0); Problems = $problems.ToArray() }
}

function Get-ResultMember($Object, [string]$Name) {
    # A member of a parsed JSON object, or $null (strict mode refuses a missing property).
    if ($null -eq $Object -or $Object -isnot [System.Management.Automation.PSCustomObject]) { return $null }
    $property = $Object.PSObject.Properties[$Name]   # strict-mode safe, also for an object without members
    if ($property) { return $property.Value }
    return $null
}

function Get-RecordedExports($Value) {
    # The export records of a run that name a file and its SHA-256. An empty object or null (what a
    # gate wrote for no file before its outputs were always arrays) and a record without a hash count
    # as none, so they can never stand for an export (W12 review CMG-03).
    return @(@($Value) | Where-Object { $null -ne $_ -and (Get-ResultMember $_ 'file') -and ([string](Get-ResultMember $_ 'sha256')) -match '^[0-9a-f]{64}$' })
}

function Test-CleanMachineResult {
    <#
    .SYNOPSIS
        Checks a gate.json (-Gate, parsed) against what the host prepared: -Photos photos (and with
        -Fixtures their file names, which the gate's photos must be), -InstallerSha256 the package's
        Setup; -VersionRehearsal when the host gave the package's version rehearsal Setup (the
        installer test must then have run with -NewerInstaller); -Rehearsal accepts an unclean, online
        machine (the environment's checks recorded, not required); -SkipInstallerTest when the gate
        ran without the installer test. Returns Passed and Problems.
    #>
    param([Parameter(Mandatory)]$Gate, [Parameter(Mandatory)][int]$Photos, [AllowEmptyCollection()][string[]]$Fixtures = @(), [string]$InstallerSha256,
          [switch]$VersionRehearsal, [switch]$Rehearsal, [switch]$SkipInstallerTest)
    $problems = New-Object System.Collections.Generic.List[string]
    if ((Get-ResultMember $Gate 'gate') -ne 'clean-machine' -or $null -eq (Get-ResultMember $Gate 'checks')) {
        return [pscustomobject]@{ Passed = $false; Problems = @('not a clean-machine gate result') }
    }
    $expectedMode = if ($Rehearsal) { 'rehearsal' } else { 'sandbox' }
    if ((Get-ResultMember $Gate 'mode') -ne $expectedMode) { $problems.Add("the gate ran as a $(Get-ResultMember $Gate 'mode'), not a $expectedMode") }
    $checks = @(Get-ResultMember $Gate 'checks')
    $byId = @{}
    foreach ($check in $checks) { $byId[[string](Get-ResultMember $check 'id')] = $check }
    foreach ($id in $script:GateCheckIds) {
        if ($SkipInstallerTest -and $id -eq 'installer-test') { continue }
        if (-not $byId.ContainsKey($id)) { $problems.Add("the gate did not report '$id'"); continue }
        $required = -not ($Rehearsal -and $id -like 'environment.*')
        if ($required -and -not [bool](Get-ResultMember $byId[$id] 'passed')) { $problems.Add("failed: $(Get-ResultMember $byId[$id] 'name') ($(Get-ResultMember $byId[$id] 'detail'))") }
    }
    foreach ($check in $checks) {
        if ($script:GateCheckIds -notcontains [string](Get-ResultMember $check 'id') -and -not [bool](Get-ResultMember $check 'passed')) {
            $problems.Add("failed: $(Get-ResultMember $check 'name') ($(Get-ResultMember $check 'detail'))")
        }
    }
    # The version rehearsal the host gave: the installer test must have upgraded to it and seen the
    # package's Setup refuse to go back (its check names -NewerInstaller).
    if ($VersionRehearsal -and -not $SkipInstallerTest -and $byId.ContainsKey('installer-test') -and
        [string](Get-ResultMember $byId['installer-test'] 'name') -notmatch '-NewerInstaller') {
        $problems.Add('the host gave the version rehearsal''s Setup, but the installer test ran without -NewerInstaller (no upgrade and refused downgrade)')
    }
    $runs = Get-ResultMember $Gate 'runs'
    $jpegs = @(Get-RecordedExports (Get-ResultMember (Get-ResultMember $runs 'run1') 'jpeg')).Count
    $tiffs = @(Get-RecordedExports (Get-ResultMember (Get-ResultMember $runs 'run2') 'tiff')).Count
    if ($jpegs -ne $Photos -or $tiffs -ne $Photos) { $problems.Add("$Photos photo(s) given, $jpegs JPEG and $tiffs TIFF export(s) recorded with their SHA-256") }
    $seen = @(@(Get-ResultMember $Gate 'fixtures') | Where-Object { $null -ne $_ -and (Get-ResultMember $_ 'file') } |
        ForEach-Object { [System.IO.Path]::GetFileName(([string](Get-ResultMember $_ 'file')).Replace('/', '\')) })
    if ($seen.Count -ne $Photos) { $problems.Add("$Photos photo(s) given, the gate saw $($seen.Count)") }
    # A request without file names (null binds as one empty name) compares none.
    $given = [string[]]@($Fixtures | Where-Object { $_ } | ForEach-Object { [string]$_ })
    if ($given.Count -gt 0) {
        $found = [string[]]@($seen)
        [Array]::Sort($given, [System.StringComparer]::Ordinal)
        [Array]::Sort($found, [System.StringComparer]::Ordinal)
        if (($given -join '|') -cne ($found -join '|')) { $problems.Add("the gate's photos ($($found -join ', ')) are not the ones the host gave ($($given -join ', '))") }
    }
    if ($InstallerSha256 -and [string](Get-ResultMember (Get-ResultMember $Gate 'installer') 'sha256') -ne $InstallerSha256) {
        $problems.Add('the gate installed another Setup than the package''s')
    }
    $passed = ($problems.Count -eq 0) -and [bool](Get-ResultMember $Gate 'passed')
    if ($problems.Count -eq 0 -and -not [bool](Get-ResultMember $Gate 'passed')) { $problems.Add('the gate says it failed') }
    return [pscustomobject]@{ Passed = $passed; Problems = $problems.ToArray() }
}

function Read-CleanMachineJson {
    <#
    .SYNOPSIS
        Reads and parses a JSON file that the sandbox writes into the mapped results folder (W12
        sandbox run SBX-2: the host once read gate.json while the guest still had it open, and failed
        with a sharing violation). Retries every -IntervalMilliseconds while the file is in use,
        missing for a moment or not complete JSON, until -TimeoutSeconds; then throws with the last
        problem.
    #>
    param([Parameter(Mandatory)][string]$Path, [ValidateRange(0, 3600)][int]$TimeoutSeconds = 120, [ValidateRange(10, 60000)][int]$IntervalMilliseconds = 1000)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $attempts = 0
    while ($true) {
        $attempts++
        $problem = $null
        try {
            $text = [System.IO.File]::ReadAllText($Path)
            if ($text.Trim()) {
                try { return ($text | ConvertFrom-Json) } catch { $problem = "not complete JSON ($($_.Exception.Message))" }
            }
            else { $problem = 'empty' }
        }
        catch [System.IO.IOException] { $problem = $_.Exception.Message }
        catch [System.UnauthorizedAccessException] { $problem = $_.Exception.Message }
        if ((Get-Date) -ge $deadline) { throw "$Path could not be read within $TimeoutSeconds s ($attempts attempt(s)): $problem" }
        Start-Sleep -Milliseconds $IntervalMilliseconds
    }
}

function ConvertFrom-WsbListOutput {
    # The sandbox ids in the output of "wsb list" (plain, or JSON with --raw): every GUID it holds.
    param([AllowEmptyCollection()][AllowNull()][string[]]$Lines)
    $ids = New-Object System.Collections.Generic.List[string]
    foreach ($match in [regex]::Matches((@($Lines) -join "`n"), '[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}')) {
        if (-not $ids.Contains($match.Value.ToLowerInvariant())) { $ids.Add($match.Value.ToLowerInvariant()) }
    }
    return $ids.ToArray()
}

function Get-LucentSandboxInstances {
    <#
    .SYNOPSIS
        The Windows Sandbox environments running on this computer, as descriptions; none when no
        sandbox runs. Windows Sandbox runs one instance at a time (Microsoft's Windows Sandbox FAQ), so
        a second start is refused and its logon command never runs (W12 review CMG-02). Asks the
        Windows Sandbox app's command line ("wsb list --raw", where wsb.exe exists) and looks for the
        processes that show a sandbox's window (WindowsSandboxRemoteSession, the app's;
        WindowsSandboxClient, the Windows feature's).
    #>
    $found = New-Object System.Collections.Generic.List[string]
    $wsb = Get-Command -Name 'wsb.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($wsb) {
        $out = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-wsb-' + [guid]::NewGuid().ToString('N') + '.txt')
        try {
            $process = Start-Process -FilePath $wsb.Source -ArgumentList @('list', '--raw') -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
            if ($process.WaitForExit(30000)) {
                foreach ($id in (ConvertFrom-WsbListOutput ([System.IO.File]::ReadAllLines($out)))) { $found.Add("sandbox $id (wsb list)") }
            }
            else { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        }
        catch { }
        finally { Remove-Item -LiteralPath $out, "$out.err" -Force -ErrorAction SilentlyContinue }
    }
    foreach ($name in 'WindowsSandboxRemoteSession', 'WindowsSandboxClient') {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) { $found.Add("$name.exe (process $($process.Id))") }
    }
    return $found.ToArray()
}

function Get-LucentSandboxHostFacts {
    <#
    .SYNOPSIS
        The host's Windows (caption, version, build with its update revision) and the Windows Sandbox
        in use: the Windows Sandbox app's package version (MicrosoftWindows.WindowsSandbox), or the
        Windows feature's WindowsSandbox.exe file version; whether its command line (wsb.exe) exists.
    #>
    $facts = [ordered]@{ computer = [string]$env:COMPUTERNAME; windows = $null; version = $null; build = $null
        sandboxApp = $null; sandboxExecutable = $null; wsbCommandLine = $false }
    try {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem
        $ubr = $null
        try { $ubr = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name UBR -ErrorAction Stop).UBR } catch { $ubr = $null }
        $facts.windows = [string]$os.Caption; $facts.version = [string]$os.Version
        $facts.build = if ($null -ne $ubr) { "$($os.BuildNumber).$ubr" } else { [string]$os.BuildNumber }
    }
    catch { $facts.windows = [System.Environment]::OSVersion.VersionString }
    try {
        $app = Get-AppxPackage -Name 'MicrosoftWindows.WindowsSandbox' -ErrorAction Stop | Select-Object -First 1
        if ($app) { $facts.sandboxApp = "$($app.Name) $($app.Version)" }
    }
    catch { $facts.sandboxApp = $null }
    $exe = Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe'
    if (Test-Path -LiteralPath $exe -PathType Leaf) { $facts.sandboxExecutable = [string](Get-Item -LiteralPath $exe).VersionInfo.FileVersion }
    $facts.wsbCommandLine = [bool](Get-Command -Name 'wsb.exe' -CommandType Application -ErrorAction SilentlyContinue)
    return $facts
}

$CleanMachineGateChecks = $script:GateCheckIds
Export-ModuleMember -Function New-LucentSandboxConfiguration, Test-LucentSandboxConfiguration, Test-CleanMachineResult, Read-CleanMachineJson,
    ConvertFrom-WsbListOutput, Get-LucentSandboxInstances, Get-LucentSandboxHostFacts -Variable CleanMachineGateChecks
